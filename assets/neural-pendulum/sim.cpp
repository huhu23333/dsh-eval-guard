#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr int N = 3;
constexpr double DT = 0.01;
constexpr int SUB = 4;
constexpr int MAX_STEPS = 3000;
constexpr double PI = 3.14159265358979323846;
constexpr double GRAV = 9.81;

struct Params {
  std::array<double, N> mass{};
  std::array<double, N> len{};
  std::array<double, N> rad{};
  std::array<double, N> inertia{};
  std::array<double, N> gcoef{};
  double damp = 0.01;
  double tau_max = 18.0;
};

struct State { std::array<double,N> theta{}; std::array<double,N> omega{}; };

double wrap(double x) {
  while (x <= -PI) x += 2*PI;
  while (x > PI) x -= 2*PI;
  return x;
}

Params sample_params(std::mt19937& rng) {
  std::uniform_real_distribution<double> um(0.7, 1.3);
  std::uniform_real_distribution<double> ul(0.45, 0.55);
  std::uniform_real_distribution<double> ud(0.005, 0.02);
  std::uniform_real_distribution<double> ut(15.0, 21.0);
  Params p;
  for (int i=0;i<N;i++) {
    p.mass[i] = um(rng);
    p.len[i] = ul(rng);
    p.rad[i] = p.len[i] * 0.5;
    p.inertia[i] = p.mass[i]*p.len[i]*p.len[i]/12.0;
  }
  p.gcoef[0] = p.mass[0]*p.rad[0] + p.len[0]*(p.mass[1]+p.mass[2]);
  p.gcoef[1] = p.mass[1]*p.rad[1] + p.len[1]*p.mass[2];
  p.gcoef[2] = p.mass[2]*p.rad[2];
  p.damp = ud(rng);
  p.tau_max = ut(rng);
  return p;
}

static std::array<std::array<double,N>,N> mass_matrix(const std::array<double,N>& q,
                                                      const Params& p) {
  std::array<std::array<double,N>,N> M{};
  for (int i=0;i<N;i++) for (int j=0;j<N;j++) {
    if (i==j) {
      double tail=0; for (int k=i+1;k<N;k++) tail += p.mass[k];
      M[i][j] = p.inertia[i] + p.mass[i]*p.rad[i]*p.rad[i]
                + p.len[i]*p.len[i]*tail;
    } else {
      int a=std::min(i,j), b=std::max(i,j);
      M[i][j] = p.len[a] * p.gcoef[b] * std::cos(q[i]-q[j]);
    }
  }
  return M;
}
static void solve3(const std::array<std::array<double,N>,N>& A, const std::array<double,N>& b, std::array<double,N>& x) {
  double a[3][4];
  for(int i=0;i<3;i++){ for(int j=0;j<3;j++) a[i][j]=A[i][j]; a[i][3]=b[i]; }
  for(int c=0;c<3;c++){
    int p=c; for(int r=c+1;r<3;r++) if(std::fabs(a[r][c])>std::fabs(a[p][c])) p=r;
    if(p!=c) for(int j=0;j<4;j++) std::swap(a[c][j],a[p][j]);
    double d=a[c][c]; if(std::fabs(d)<1e-15) d=(d<0?-1e-15:1e-15);
    for(int j=c;j<4;j++) a[c][j]/=d;
    for(int r=0;r<3;r++) if(r!=c){ double f=a[r][c]; for(int j=c;j<4;j++) a[r][j]-=f*a[c][j]; }
  }
  for(int i=0;i<3;i++) x[i]=a[i][3];
}
static void deriv(const State& s, const std::array<double,N>& tau,
                  const Params& p, State& out) {
  auto M = mass_matrix(s.theta, p);
  std::array<double,N> C{}, G{}, Q{}, rhs{};
  for(int i=0;i<N;i++){
    double ci=0;
    for(int j=0;j<N;j++) if(j!=i) {
      const int a=std::min(i,j), b=std::max(i,j);
      ci += p.len[a] * p.gcoef[b] * std::sin(s.theta[i]-s.theta[j]) * s.omega[j]*s.omega[j];
    }
    C[i]=ci;
    G[i]=GRAV*std::sin(s.theta[i])*p.gcoef[i];
  }
  std::array<double,N> edge{};
  for(int i=0;i<N;i++) edge[i] = s.omega[i] - (i>0?s.omega[i-1]:0.0);
  for(int i=0;i<N;i++){
    double e = edge[i] - (i+1<N ? edge[i+1] : 0.0);
    Q[i] = -p.damp*e;
  }
  for(int i=0;i<N;i++) rhs[i]=tau[i]-C[i]-G[i]+Q[i];
  solve3(M, rhs, out.omega);
  out.theta = s.omega;
}
} // namespace

int main(int argc, char** argv) {
#ifdef DSH_EVAL_SIM
  /* Evaluator-only binary.  Every reset samples a fresh hidden plant and a
   * fresh hidden observation-noise level from a deterministic internal RNG.
   * It also reports the true joint state for the evaluator's success check;
   * the candidate controller only receives the noisy observation. */
  uint32_t seed = 20261024u;
  std::mt19937 rng(seed);
#else
  /* Agent/training binary.  Every reset samples a fresh hidden plant, random
   * near-down initial state and fresh observation-noise level. */
  uint32_t seed=0;
  for(int i=1;i<argc;i++){
    std::string a=argv[i];
    if(a=="--seed" && i+1<argc) seed=static_cast<uint32_t>(std::strtoul(argv[++i],nullptr,10));
  }
  std::mt19937 rng(seed);
#endif
  std::uniform_real_distribution<double> uinit(-0.15, 0.15);
  std::uniform_real_distribution<double> uvel(-0.2, 0.2);
  std::uniform_real_distribution<double> unoise(0.002, 0.008);
  std::normal_distribution<double> gauss(0.0, 1.0);

  Params p = sample_params(rng);
  State s;
  double noise_std = 0.0;
  int steps=0; bool ready=false;

  auto reset=[&](){
    steps=0; ready=true;
    p = sample_params(rng);
    noise_std = unoise(rng);
    for (int i=0;i<N;i++) {
      s.theta[i] = uinit(rng);
      s.omega[i] = uvel(rng);
    }
  };
  auto noisy_obs=[&](){
    std::array<double,N> o{};
    for (int i=0;i<N;i++) o[i] = wrap(s.theta[i]-PI) + gauss(rng)*noise_std;
    return o;
  };
  auto emit_state=[&](){
    std::cout << "{\"obs\":[" << noisy_obs()[0] << "," << noisy_obs()[1] << "," << noisy_obs()[2] << "]";
#ifdef DSH_EVAL_SIM
    std::cout << ",\"state\":[" << wrap(s.theta[0]-PI) << "," << wrap(s.theta[1]-PI) << "," << wrap(s.theta[2]-PI) << "]";
#endif
    std::cout << "}\n" << std::flush;
  };

  s.theta={0,0,0}; s.omega={0,0,0};
  std::string line;
  while(std::getline(std::cin,line)){
    if(line.empty()) continue;
    if(line.find("reset")!=std::string::npos){ reset(); emit_state(); continue; }
    if(line.find("step")!=std::string::npos){
      if(!ready || steps>=MAX_STEPS){ std::cout << "{\"error\":\"episode_finished\"}\n" << std::flush; continue; }
      double u1=0,u3=0; auto pos=line.find('['); if(pos!=std::string::npos){ char* end=nullptr; u1=std::strtod(line.c_str()+pos+1,&end); if(end){ const char* q=end; while(*q && (*q==','||*q==' ')) q++; u3=std::strtod(q,&end); } }
      u1=std::max(-1.0,std::min(1.0,u1)); u3=std::max(-1.0,std::min(1.0,u3));
      std::array<double,N> tau{{u1*p.tau_max,-u3*p.tau_max,u3*p.tau_max}};
      double h=DT/SUB;
      for(int sub=0;sub<SUB;sub++){
        State k1=s,k2=s,k3=s,k4=s; State d{};
        deriv(k1,tau,p,d); for(int i=0;i<N;i++){ k2.theta[i]=s.theta[i]+0.5*h*d.theta[i]; k2.omega[i]=s.omega[i]+0.5*h*d.omega[i]; }
        deriv(k2,tau,p,d); for(int i=0;i<N;i++){ k3.theta[i]=s.theta[i]+0.5*h*d.theta[i]; k3.omega[i]=s.omega[i]+0.5*h*d.omega[i]; }
        deriv(k3,tau,p,d); for(int i=0;i<N;i++){ k4.theta[i]=s.theta[i]+h*d.theta[i]; k4.omega[i]=s.omega[i]+h*d.omega[i]; }
        State ds1{},ds2{},ds3{},ds4{};
        deriv(k1,tau,p,ds1); deriv(k2,tau,p,ds2); deriv(k3,tau,p,ds3); deriv(k4,tau,p,ds4);
        for(int i=0;i<N;i++){
          s.theta[i] += h/6.0*(ds1.theta[i]+2*ds2.theta[i]+2*ds3.theta[i]+ds4.theta[i]);
          s.omega[i] += h/6.0*(ds1.omega[i]+2*ds2.omega[i]+2*ds3.omega[i]+ds4.omega[i]);
        }
      }
      steps++;
      emit_state();
      continue;
    }
    std::cout << "{\"error\":\"bad_command\"}\n" << std::flush;
  }
  return 0;
}
