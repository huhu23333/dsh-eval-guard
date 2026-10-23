#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
constexpr double DAMP = 0.01;
constexpr double TAU_MAX = 18.0;
const std::array<double, N> MASS{{1.0, 1.0, 1.0}};
const std::array<double, N> LEN{{0.5, 0.5, 0.5}};
const std::array<double, N> R{{0.25, 0.25, 0.25}};
const std::array<double, N> INERTIA{{MASS[0]*LEN[0]*LEN[0]/12.0, MASS[1]*LEN[1]*LEN[1]/12.0, MASS[2]*LEN[2]*LEN[2]/12.0}};
const std::array<double, N> GCOEF{{MASS[0]*R[0] + LEN[0]*(MASS[1]+MASS[2]),
                                   MASS[1]*R[1] + LEN[1]*MASS[2],
                                   MASS[2]*R[2]}};

struct Vec { std::array<double,N> v{}; };
struct State { std::array<double,N> theta{}; std::array<double,N> omega{}; };

double wrap(double x) {
  while (x <= -PI) x += 2*PI;
  while (x > PI) x -= 2*PI;
  return x;
}

static std::array<std::array<double,N>,N> mass_matrix(const std::array<double,N>& q) {
  std::array<std::array<double,N>,N> M{};
  for (int i=0;i<N;i++) for (int j=0;j<N;j++) {
    if (i==j) {
      double tail=0; for (int k=i+1;k<N;k++) tail += MASS[k];
      M[i][j] = INERTIA[i] + MASS[i]*R[i]*R[i] + LEN[i]*LEN[i]*tail;
    } else {
      int a=std::min(i,j), b=std::max(i,j);
      M[i][j] = LEN[a] * GCOEF[b] * std::cos(q[i]-q[j]);
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
static void deriv(const State& s, const std::array<double,N>& tau, State& out) {
  auto M = mass_matrix(s.theta);
  std::array<double,N> C{}, G{}, Q{}, rhs{};
  for(int i=0;i<N;i++){
    double ci=0;
    for(int j=0;j<N;j++) if(j!=i) {
      // Coriolis coefficient: d(M_ij)/d(theta_j) = LEN[min] * GCOEF[max] * sin(...);
      // M_ij itself already contains cos(...), so multiplying M_ij by sin would
      // add an extra cos factor and inject energy at high speed (NaN after ~7 s).
      const int a=std::min(i,j), b=std::max(i,j);
      ci += LEN[a] * GCOEF[b] * std::sin(s.theta[i]-s.theta[j]) * s.omega[j]*s.omega[j];
    }
    C[i]=ci;
    G[i]=GRAV*std::sin(s.theta[i])*GCOEF[i];
  }
  // joint damping: b * D^T D omega, D lower bidiagonal with -1 previous
  std::array<double,N> edge{};
  for(int i=0;i<N;i++) edge[i] = s.omega[i] - (i>0?s.omega[i-1]:0.0);
  for(int i=0;i<N;i++){
    double e = edge[i] - (i+1<N ? edge[i+1] : 0.0);
    Q[i] = -DAMP*e;
  }
  for(int i=0;i<N;i++) rhs[i]=tau[i]-C[i]-G[i]+Q[i];
  solve3(M, rhs, out.omega);
  out.theta = s.omega;
}
} // namespace

int main(int argc, char** argv) {
#ifdef DSH_EVAL_SIM
  /* Evaluator-only binary: every reset MUST use the fixed evaluation state.
   * This build deliberately contains no random training reset path. */
  (void)argc;
  (void)argv;
#else
  /* Agent/training binary: every reset is a random near-down state.  This
   * build deliberately contains no fixed evaluation state and no --eval flag,
   * so the agent cannot query the evaluation initial condition from it. */
  int seed=0;
  for(int i=1;i<argc;i++){
    std::string a=argv[i];
    if(a=="--seed" && i+1<argc) seed=std::atoi(argv[++i]);
  }
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> u(-1.0,1.0);
#endif
  State s;
  int steps=0; bool ready=false;
  auto reset=[&](){
    steps=0; ready=true;
#ifdef DSH_EVAL_SIM
    s.theta={0.013,-0.008,0.021};
    s.omega={0,0,0};
#else
    s.theta={u(rng)*0.05,u(rng)*0.05,u(rng)*0.05};
    s.omega={u(rng)*0.02,u(rng)*0.02,u(rng)*0.02};
#endif
  };
  // zero state
  s.theta={0,0,0}; s.omega={0,0,0};
  std::string line;
  while(std::getline(std::cin,line)){
    if(line.empty()) continue;
    if(line.find("reset")!=std::string::npos){ reset(); std::cout << "{\"obs\":[" << wrap(s.theta[0]-PI) << "," << wrap(s.theta[1]-PI) << "," << wrap(s.theta[2]-PI) << "]}\n" << std::flush; continue; }
    if(line.find("step")!=std::string::npos){
      if(!ready || steps>=MAX_STEPS){ std::cout << "{\"error\":\"episode_finished\"}\n" << std::flush; continue; }
      // parse torque array
      double u1=0,u3=0; auto p=line.find('['); if(p!=std::string::npos){ char* end=nullptr; u1=std::strtod(line.c_str()+p+1,&end); if(end){ const char* q=end; while(*q && (*q==','||*q==' ')) q++; u3=std::strtod(q,&end); } }
      u1=std::max(-1.0,std::min(1.0,u1)); u3=std::max(-1.0,std::min(1.0,u3));
      // Joint 3 is an internal motor between link 2 and link 3. In absolute
      // coordinates its generalized force is (tau1, -tau3, tau3): the reaction
      // on link 2 is part of the same joint torque. (tau1, 0, tau3) would be a
      // reactionless external torque on link 3 and would contradict joint 2
      // being passive.
      std::array<double,N> tau{{u1*TAU_MAX,-u3*TAU_MAX,u3*TAU_MAX}};
      // one control step with 4 RK4 substeps
      double h=DT/SUB;
      for(int sub=0;sub<SUB;sub++){
        State k1=s,k2=s,k3=s,k4=s; State d{};
        deriv(k1,tau,d); for(int i=0;i<N;i++){ k2.theta[i]=s.theta[i]+0.5*h*d.theta[i]; k2.omega[i]=s.omega[i]+0.5*h*d.omega[i]; }
        deriv(k2,tau,d); for(int i=0;i<N;i++){ k3.theta[i]=s.theta[i]+0.5*h*d.theta[i]; k3.omega[i]=s.omega[i]+0.5*h*d.omega[i]; }
        deriv(k3,tau,d); for(int i=0;i<N;i++){ k4.theta[i]=s.theta[i]+h*d.theta[i]; k4.omega[i]=s.omega[i]+h*d.omega[i]; }
        deriv(k4,tau,d);
        // need derivatives at all; use helper recompute arrays
        State ds1{},ds2{},ds3{},ds4{};
        deriv(k1,tau,ds1); deriv(k2,tau,ds2); deriv(k3,tau,ds3); deriv(k4,tau,ds4);
        for(int i=0;i<N;i++){
          s.theta[i] += h/6.0*(ds1.theta[i]+2*ds2.theta[i]+2*ds3.theta[i]+ds4.theta[i]);
          s.omega[i] += h/6.0*(ds1.omega[i]+2*ds2.omega[i]+2*ds3.omega[i]+ds4.omega[i]);
        }
      }
      steps++;
      std::cout << "{\"obs\":[" << wrap(s.theta[0]-PI) << "," << wrap(s.theta[1]-PI) << "," << wrap(s.theta[2]-PI) << "]}\n" << std::flush;
      continue;
    }
    std::cout << "{\"error\":\"bad_command\"}\n" << std::flush;
  }
  return 0;
}
