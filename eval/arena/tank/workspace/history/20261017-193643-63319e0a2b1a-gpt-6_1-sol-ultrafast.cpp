// Coordinated tank bot: supply control, a committed fast flanker, and adaptive support.
// Supply assignments account for opposing arrival times and chassis-specific contest risk.
// Fire tanks discount incidental chases while retaining critical defensive interceptions.
// Exact joint-action simulation combines strategic rollouts with nested simultaneous games.
// Shared action rankings avoid repeated work; every search retains a legal budget fallback.
#include "tank.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>
#include <cstdio>
#include <unordered_map>
using namespace tank;
namespace bot {
constexpr float INF=10000;
float bd[6][CELLS], sd[6][MAX_SUPPLY][CELLS], hd[6][3][CELLS], field[6][CELLS];
int goal[6], previous[6];float anchor[6];
uint64_t rng=123456789;
uint64_t random64(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
float rand01(){return (random64()>>40)*(1.0f/16777216);}
int md(int a,int b){return std::abs(cell_x(a)-cell_x(b))+std::abs(cell_y(a)-cell_y(b));}
int direction(int a,int b){if(a==b)return -1;if(cell_x(a)==cell_x(b))return a>b?0:1;if(cell_y(a)==cell_y(b))return a>b?2:3;return -1;}
bool clear(const State&s,int a,int b){int d=direction(a,b);if(d<0)return false;for(int p=a+DIR_DELTA[d];p!=b;p+=DIR_DELTA[d])if(s.grid[p]!=EMPTY&&s.grid[p]!=WATER)return false;return true;}
void dijkstra(const State&s,int i,float *dist,const std::vector<std::pair<int,float>>& seeds){
 for(int p=0;p<CELLS;p++)dist[p]=INF;
 using Node=std::pair<float,int>;
 std::priority_queue<Node,std::vector<Node>,std::greater<Node>>q;
 for(auto [p,v]:seeds)if(v<dist[p]){dist[p]=v;q.emplace(v,p);}
 float walk=type_of(i)==FAST?.58f:1.f;
 while(!q.empty()){
  auto [v,p]=q.top();q.pop();if(v>dist[p]+.001f)continue;
  int g=s.grid[p];float extra=g==BRICK?2.f:g==STEEL?3.f:0.f;
  for(int d=0;d<4;d++){
   int n=p+DIR_DELTA[d];if(n<0||n>=CELLS)continue;
   int ng=s.grid[n];if(ng==OUTSIDE||ng==WATER||ng==BASE0||ng==BASE1||(ng==STEEL&&s.tank[i].level<2))continue;
   float nv=v+walk+extra;
   if(nv<dist[n]){dist[n]=nv;q.emplace(nv,n);}
  }
 }
}
void prepare(const State&s){
 for(int i=0;i<6;i++){
  int side=seat_of(i),base=s.base_pos[1-side];std::vector<std::pair<int,float>> seeds;
  for(int d=0;d<4;d++){
   int walls=0,range=0;
   for(int p=base+DIR_DELTA[d];p>=0&&p<CELLS;p+=DIR_DELTA[d]){
    int g=s.grid[p];if(g==OUTSIDE||g==BASE0||g==BASE1||(g==STEEL&&s.tank[i].level<2))break;
    range++;if(g==BRICK||g==STEEL)walls++;
    if(g!=WATER)seeds.emplace_back(p,range*(type_of(i)==FAST?1.45f:.85f)+walls*2.0f+((type_of(i)==FAST&&d<2)?5.f:0.f));
   }
  }
  dijkstra(s,i,bd[i],seeds);
  for(int k=0;k<s.n_supply;k++)dijkstra(s,i,sd[i][k],{{s.supply[k],0}});
 }
 for(int i=0;i<6;i++)for(int t=0;t<3;t++){
  int j=(1-seat_of(i))*3+t,p=s.tank[j].pos;
  std::vector<std::pair<int,float>> seeds;
  if(s.tank[j].alive)for(int d=0;d<4;d++)for(int range=1;range<=5;range++){
   int q=p+DIR_DELTA[d]*range;if(q<0||q>=CELLS||s.grid[q]==OUTSIDE)break;
   int g=s.grid[q];if(g==BASE0||g==BASE1)break;if(g==STEEL&&s.tank[i].level<2)break;
   if(g==EMPTY||g==BRICK||g==STEEL)seeds.emplace_back(q,1.0f*range+(g==EMPTY?0.f:2.f));
   if(g==BRICK||g==STEEL)break;
  }
  dijkstra(s,i,hd[i][t],seeds);
 }
 for(int side=0;side<2;side++){
  float options[3][7];int next=s.turn<10?10-s.turn:20-(s.turn-10)%20;
  for(int t=0;t<3;t++){
   int i=side*3+t;int p=s.tank[i].alive?s.tank[i].pos:s.spawn[i];
   options[t][0]=-bd[i][p]*.65f;
   for(int k=0;k<s.n_supply;k++){
    float wait=(s.star_mask&(1<<k))?0.f:float(next+1);
    float eta=sd[i][k][p],enemy=INF;
    for(int j=(1-side)*3;j<(1-side)*3+3;j++)if(s.tank[j].alive&&s.tank[j].level<2)enemy=std::min(enemy,sd[j][k][s.tank[j].pos]);
    float value=(s.tank[i].level==0?17.f:0.f)-1.5f*std::max(eta,wait);
    if(enemy+.5f<eta)value-=std::min(18.f,(eta-enemy)*(t==0?4.f:t==2?1.f:2.f));
    if(!s.tank[i].alive)value=-INF;
    if(s.tank[i].level>=2)value=-INF;
    options[t][k+1]=value;
   }
   for(int e=0;e<3;e++){
    int j=(1-side)*3+e;
    float threat=-INF;
    if(s.tank[j].alive){
     int dist=md(s.tank[j].pos,s.base_pos[side]);
     if(dist<=7){threat=27.f-2.f*dist+3.f*s.tank[j].level;
      if(clear(s,s.tank[j].pos,s.base_pos[side]))threat+=18.f;
     }
    }
    options[t][4+e]=threat-1.5f*hd[i][e][p]-(t==1?8.f:0.f);
    if(t==0||!s.tank[i].alive)options[t][4+e]=-INF;
   }
  }
  float best=-INF*10;int choice[3]={};
  for(int a=0;a<7;a++)for(int b=0;b<7;b++)for(int c=0;c<7;c++){
   if((a&&a==b)||(a&&a==c)||(b&&b==c))continue;
   float v=options[0][a]+options[1][b]+options[2][c];
   if(v>best){best=v;choice[0]=a;choice[1]=b;choice[2]=c;}
  }
  for(int t=0;t<3;t++){
   int i=side*3+t;goal[i]=choice[t]-1;
   for(int p=0;p<CELLS;p++)field[i][p]=goal[i]<0?-bd[i][p]:goal[i]<3?-sd[i][goal[i]][p]:-hd[i][goal[i]-3][p];
  }
 }
}
float projectile(const State&s,const Bullet&b){
 int side=seat_of(b.owner),p=b.pos,steps=0;
 for(;;){
  if(p<0||p>=CELLS)return 0;
  int g=s.grid[p];
  if(g==OUTSIDE||g==BRICK||g==STEEL)return 0;
  if(g==BASE0||g==BASE1){
   float value=60.f/(1.f+.08f*steps);
   for(int j=0;j<6;j++)if(seat_of(j)!=side&&s.tank[j].alive){int dd=md(s.tank[j].pos,p);if(dd<steps/2+2)value*=.72f;}
   return (g-BASE0==side?-1.f:1.f)*value;
  }
  for(int j=0;j<6;j++)if(s.tank[j].alive&&s.tank[j].pos==p){
   int escapes=0;for(int d=0;d<4;d++){int n=p+DIR_DELTA[d];if(s.grid[n]==EMPTY&&tank_at(s,n)<0)escapes++;}
   float value=(s.tank[j].hp==1?19.f:9.f)/(1.f+.35f*steps);
   if(steps>0)value*=escapes? .3f:.9f;
   if(steps>0&&can_fire(s,j)&&direction(p,b.pos)>=0)value*=.6f;
   return (seat_of(j)==side?-1.f:1.f)*value;
  }
  p+=DIR_DELTA[b.dir];steps++;
 }
}
float evaluate(const State&s,int side){
 if(s.done)return s.winner<0?0.f:(s.winner==side?100000.f:-100000.f);
 float v=140.f*(s.base_hp[side]-s.base_hp[1-side])+30.f*(s.kills[side]-s.kills[1-side])+4.f*(s.stars_eaten[side]-s.stars_eaten[1-side]);
 for(int i=0;i<6;i++){
  auto&t=s.tank[i];float val;
  if(t.alive){
   val=34.f+11.f*t.hp+14.f*t.level;
   val+=5.f*(std::max(-45.f,field[i][t.pos])-anchor[i]);
   if(clear(s,t.pos,s.base_pos[1-seat_of(i)]))val+=25.f/(1.f+.18f*md(t.pos,s.base_pos[1-seat_of(i)]));
  }else val=-4.f*respawn_in(s,i)-20.f;
  v+=(seat_of(i)==side?1.f:-1.f)*val;
 }
 for(int k=0;k<s.n_bullets;k++)v+=(seat_of(s.bullet[k].owner)==side?1.f:-1.f)*projectile(s,s.bullet[k]);
 return v;
}
void path(const State&s,int i,Cmd c,int&p1,int&p2){
 p1=p2=s.tank[i].pos;
 int d1=-1,d2=-1;if(is_move(c))d1=c-1;else if(is_move2(c)){d1=(c-9)/4;d2=(c-9)%4;}
 if(d1>=0){int n=p1+DIR_DELTA[d1];if(s.grid[n]==EMPTY&&tank_at(s,n)<0)p1=n;}
 p2=p1;if(d2>=0){int n=p2+DIR_DELTA[d2];if(s.grid[n]==EMPTY&&tank_at(s,n)<0)p2=n;}
}
float shot_value(const State&s,int i,int d){
 int p=s.tank[i].pos,side=seat_of(i),steps=0;
 for(int n=p+DIR_DELTA[d];;n+=DIR_DELTA[d]){
  steps++;int g=s.grid[n];if(g==OUTSIDE)return 0;
  if(g==BASE0||g==BASE1)return g-BASE0==side?-200.f:70.f/(1.f+.08f*steps);
  int j=tank_at(s,n);
  if(j>=0){
   if(seat_of(j)==side)return -25.f/(1.f+.1f*steps);
   int exits=0;for(int k=0;k<4;k++)if(s.grid[n+DIR_DELTA[k]]==EMPTY&&tank_at(s,n+DIR_DELTA[k])<0)exits++;
   float value=(s.tank[j].hp==1?20.f:13.f)/(1+.35f*steps);
   if(exits>1)value*=.65f;
   if(can_fire(s,j))value*=.65f;
   return value;
  }
  if(g==BRICK||g==STEEL){
   if(g==STEEL&&s.tank[i].level<2)return 0;
   float gain=field[i][n]-field[i][p];
   float value=std::max(0.f,2.0f*gain-1.f)/(1.f+.2f*steps);
   int base=s.base_pos[1-side];
   if(direction(p,base)==d){
    bool line=true;for(int q=n+DIR_DELTA[d];q!=base;q+=DIR_DELTA[d]){if(s.grid[q]==OUTSIDE||(s.grid[q]==STEEL&&s.tank[i].level<2)){line=false;break;}}
    if(line)value+=12.f/(1.f+.1f*md(p,base));
   }
   int own=s.base_pos[side];if(md(n,own)<=2&&md(p,own)<6)value-=8.f;
   return value;
  }
 }
}
float primitive(const State&s,int i,Cmd c){
 if(!s.tank[i].alive)return 0;
 int p=s.tank[i].pos,p1,p2;path(s,i,c,p1,p2);
 float val=3.2f*(field[i][p2]-field[i][p]);
 bool fire=is_fire(c)&&can_fire(s,i);int fd=fire?c-5:-1;
 if(fire)val+=shot_value(s,i,fd);
 if(c==STAY)val-=.12f;
 if(p2==p&&is_move(c))val-=.4f;
 int damage=0;
 for(int k=0;k<s.n_bullets;k++){
  const auto&b=s.bullet[k];int n=b.pos+DIR_DELTA[b.dir],nn=n+DIR_DELTA[b.dir];
  bool cancel=false;
  if(fire){int f1=p+DIR_DELTA[fd],f2=f1+DIR_DELTA[fd];cancel=(f1==n)||(f1==b.pos&&n==p)||(f2==nn)||(f2==n&&nn==f1);}
  if(cancel)continue;
  bool hit=n==p1||(n==p&&b.pos==p1);
  if(!hit&&s.grid[n]!=BRICK&&s.grid[n]!=STEEL&&s.grid[n]!=OUTSIDE&&tank_at(s,n)<0)hit=nn==p2||(nn==p1&&n==p2);
  if(hit)damage++;
 }
 if(damage)val-=damage*17.f+(damage>=s.tank[i].hp?65.f+15*s.tank[i].level:0.f);
 for(int j=0;j<6;j++)if(seat_of(i)!=seat_of(j)&&s.tank[j].alive&&can_fire(s,j)){
  int e=s.tank[j].pos;
  for(int d=0;d<4;d++){
   int n=e+DIR_DELTA[d],nn=n+DIR_DELTA[d];
   bool hit=(n==p1)||(nn==p2&&s.grid[n]!=BRICK&&s.grid[n]!=STEEL&&tank_at(s,n)<0)||(p1==e&&n==p);
   if(hit){if(fire&&fd==(d^1)&&direction(p,e)==fd)continue;val-=(s.tank[i].hp==1?17.f:7.f);}
  }
 }
 for(int j=0;j<6;j++)if(seat_of(j)!=seat_of(i)&&!s.tank[j].alive&&s.spawn[j]==p2&&respawn_in(s,j)<=1)val-=200;
 if(goal[i]>=0&&goal[i]<3&&p2==s.supply[goal[i]]&&s.tank[i].level<2&&(s.star_mask&(1<<goal[i])))val+=28;
 if(p2==previous[i]&&p2!=p)val-=.18f;
 return val;
}
struct Rated {Cmd c;float v;};
struct Plan {Orders o;float v;};
std::vector<Rated> ranked(const State&s,int i){
 Cmd a[N_CMDS];int n=legal_commands(s,i,a);std::vector<Rated>r;
 for(int k=0;k<n;k++)r.push_back({a[k],primitive(s,i,a[k])});
 std::stable_sort(r.begin(),r.end(),[](auto&a,auto&b){return a.v>b.v;});return r;
}
Orders policy_from_r(const State&s,int side,const std::vector<Rated>* rs){
 Orders o;int p1[3],p2[3];
 for(int t=0;t<3;t++){
  int i=side*3+t;const auto&r=rs[t];float best=-INF;
  for(auto x:r){int a,b;path(s,i,x.c,a,b);float v=x.v;
   for(int j=0;j<t;j++)if(s.tank[side*3+j].alive&&s.tank[i].alive){if(a==p1[j])v-=20;if(b==p2[j])v-=20;}
   if(v>best){best=v;o.cmd[t]=x.c;p1[t]=a;p2[t]=b;}
  }
 }
 return o;
}
Orders policy(const State&s,int side){
 std::vector<Rated> rs[3];for(int t=0;t<3;t++)rs[t]=ranked(s,side*3+t);
 return policy_from_r(s,side,rs);
}
std::vector<Orders> plans(const State&s,int side){
 std::vector<Rated> r[3];for(int t=0;t<3;t++)r[t]=ranked(s,side*3+t);
 std::vector<Plan> all;
 for(int a=0;a<std::min(4,(int)r[0].size());a++)for(int b=0;b<std::min(4,(int)r[1].size());b++)for(int c=0;c<std::min(4,(int)r[2].size());c++){
  Orders o;o.cmd[0]=r[0][a].c;o.cmd[1]=r[1][b].c;o.cmd[2]=r[2][c].c;
  int p1[3],p2[3];float v=r[0][a].v+r[1][b].v+r[2][c].v;
  for(int t=0;t<3;t++)path(s,side*3+t,o.cmd[t],p1[t],p2[t]);
  for(int t=0;t<3;t++)for(int j=t+1;j<3;j++)if(s.tank[side*3+t].alive&&s.tank[side*3+j].alive){if(p1[t]==p1[j])v-=20;if(p2[t]==p2[j])v-=20;}
  all.push_back({o,v});
 }
 std::stable_sort(all.begin(),all.end(),[](auto&a,auto&b){return a.v>b.v;});
 std::vector<Orders>out;
 auto add=[&](Orders o){for(auto q:out)if(q==o)return;out.push_back(o);};
 add(policy_from_r(s,side,r));for(int k=0;k<std::min(14,(int)all.size());k++)add(all[k].o);
 Orders best=out[0];
 for(int t=0;t<3;t++)for(int a=0;a<std::min(7,(int)r[t].size());a++){Orders o=best;o.cmd[t]=r[t][a].c;add(o);}
 add(Orders{});return out;
}
std::vector<Orders> smallplans(const State&s,int side){
 std::vector<Rated> rs[3];for(int t=0;t<3;t++)rs[t]=ranked(s,side*3+t);
 Orders base=policy_from_r(s,side,rs);std::vector<Orders> a{base};
 for(int t=0;t<3;t++){
  const auto&r=rs[t];int got=0;
  for(auto x:r)if(x.c!=base.cmd[t]){Orders q=base;q.cmd[t]=x.c;if(std::find(a.begin(),a.end(),q)==a.end())a.push_back(q);if(++got>=2)break;}
 }
 return a;
}
float subgame(const State&s,int side){
 if(s.done)return evaluate(s,side);
 auto a=smallplans(s,side),b=smallplans(s,1-side);int n=a.size(),m=b.size();
 float M[8][8];for(int i=0;i<n;i++)for(int j=0;j<m;j++){State z=s;step_as(z,side,a[i],b[j]);M[i][j]=evaluate(z,side);}
 double u[8]={},v[8]={},ca[8]={},cb[8]={};int x=0,y=0;
 for(int it=0;it<160;it++){
  if(it>=40){ca[x]++;cb[y]++;}
  for(int i=0;i<n;i++)u[i]+=M[i][y];
  for(int j=0;j<m;j++)v[j]+=M[x][j];
  x=std::max_element(u,u+n)-u;y=std::min_element(v,v+m)-v;
 }
 double ans=0;for(int i=0;i<n;i++)for(int j=0;j<m;j++)ans+=ca[i]*cb[j]*M[i][j];return ans/14400;
}
struct GameMix {std::vector<double> a,b;};
GameMix solvegame(const std::vector<std::vector<float>>&M){
 int n=M.size(),m=M[0].size();
 std::vector<double>u(n,0),v(m,0),ca(n,0),cb(m,0);int x=0,y=0;
 for(int it=0;it<1200;it++){
  if(it>=300){ca[x]++;cb[y]++;}
  for(int j=0;j<n;j++)u[j]+=M[j][y];
  for(int j=0;j<m;j++)v[j]+=M[x][j];
  x=std::max_element(u.begin(),u.end())-u.begin();y=std::min_element(v.begin(),v.end())-v.begin();
 }
 for(double &x:ca)x/=900;
 for(double &x:cb)x/=900;
 return {ca,cb};
}
uint64_t code(Orders o){return o.cmd[0]|(uint64_t(o.cmd[1])<<8)|(uint64_t(o.cmd[2])<<16);}
Orders choose_oracle(const State&s,int side){
 prepare(s);for(int i=0;i<6;i++)anchor[i]=std::max(-45.f,field[i][s.tank[i].alive?s.tank[i].pos:s.spawn[i]]);auto a=plans(s,side),b=plans(s,1-side);
 std::unordered_map<uint64_t,float>cache;cache.reserve(4000);
 auto ev=[&](Orders a,Orders b){
  uint64_t key=code(a)|(code(b)<<24);auto it=cache.find(key);if(it!=cache.end())return it->second;
  State z=s;step_as(z,side,a,b);float first=evaluate(z,side);
  for(int h=0;h<2&&!z.done;h++){Orders u=policy(z,side),v=policy(z,1-side);step_as(z,side,u,v);}
  float ans=.3f*first+.7f*evaluate(z,side);cache.emplace(key,ans);return ans;
 };
 GameMix mix;
 for(int round=0;round<3;round++){
  std::vector<std::vector<float>>M(a.size(),std::vector<float>(b.size()));
  for(int x=0;x<(int)a.size();x++)for(int y=0;y<(int)b.size();y++)M[x][y]=ev(a[x],b[y]);
  mix=solvegame(M);if(round==2||budget_left()<18)break;
  std::vector<std::pair<double,Orders>>sa,sb;
  for(int x=0;x<(int)a.size();x++)if(mix.a[x]>.001)sa.emplace_back(mix.a[x],a[x]);
  for(int y=0;y<(int)b.size();y++)if(mix.b[y]>.001)sb.emplace_back(mix.b[y],b[y]);
  auto prep=[](auto&v){std::sort(v.begin(),v.end(),[](auto&a,auto&b){return a.first>b.first;});if(v.size()>6)v.resize(6);double sum=0;for(auto &x:v)sum+=x.first;for(auto &x:v)x.first/=sum;};prep(sa);prep(sb);
  auto response=[&](int who,Orders best,const std::vector<std::pair<double,Orders>>&support){
   auto val=[&](Orders o){double v=0;for(auto &[p,e]:support)v+=p*(who==side?ev(o,e):-ev(e,o));return v;};
   double current=val(best);
   for(int pass=0;pass<2;pass++)for(int t=0;t<3;t++){
    if(budget_left()<7)return best;
    Cmd list[N_CMDS];int n=legal_commands(s,who*3+t,list);Orders old=best;
    for(int c=0;c<n;c++){Orders trial=old;trial.cmd[t]=list[c];double v=val(trial);if(v>current+.001){current=v;best=trial;}}
   }
   return best;
  };
  int ai=std::max_element(mix.a.begin(),mix.a.end())-mix.a.begin(),bi=std::max_element(mix.b.begin(),mix.b.end())-mix.b.begin();
  Orders ar=response(side,a[ai],sb),br=response(1-side,b[bi],sa);
  if(std::find(a.begin(),a.end(),ar)==a.end())a.push_back(ar);
  if(std::find(b.begin(),b.end(),br)==b.end())b.push_back(br);
 }
 double target=rand01(),sum=0;for(int x=0;x<(int)a.size();x++){sum+=mix.a[x];if(sum>=target)return a[x];}
 return a.back();
}
Orders choose_nested(const State&s,int side){
 prepare(s);for(int i=0;i<6;i++)anchor[i]=std::max(-45.f,field[i][s.tank[i].alive?s.tank[i].pos:s.spawn[i]]);
 auto a=plans(s,side),b=plans(s,1-side);
 int n=a.size(),m=b.size();std::vector<float>matrix(n*m);
 for(int x=0;x<n;x++)for(int y=0;y<m;y++){
  if((y&3)==0&&budget_left()<5)return policy(s,side);
  State z=s;step_as(z,side,a[x],b[y]);float first=evaluate(z,side);
  matrix[x*m+y]=.15f*first+.85f*subgame(z,side);
 }
 std::vector<double>u(n,0),v(m,0),counts(n,0),opp(m,0);int x=0,y=0;
 for(int it=0;it<220;it++){
  counts[x]++;opp[y]++;
  for(int j=0;j<n;j++)u[j]+=matrix[j*m+y];
  for(int j=0;j<m;j++)v[j]+=matrix[x*m+j];
  x=std::max_element(u.begin(),u.end())-u.begin();y=std::min_element(v.begin(),v.end())-v.begin();
 }
 double target=rand01()*220,acc=0;int chosen=n-1;
 for(int j=0;j<n;j++){acc+=counts[j];if(acc>target){chosen=j;break;}}
 return a[chosen];
}
Orders choose(const State&s,int side){
 bool combat=false;
 for(int i=side*3;i<side*3+3;i++)if(s.tank[i].alive){
  for(int j=(1-side)*3;j<(1-side)*3+3;j++)if(s.tank[j].alive){
   int d=md(s.tank[i].pos,s.tank[j].pos);
   if(d<=3||(d<=6&&clear(s,s.tank[i].pos,s.tank[j].pos)))combat=true;
  }
  for(int k=0;k<s.n_bullets;k++)if(md(s.bullet[k].pos,s.tank[i].pos)<=4)combat=true;
 }
 return combat?choose_nested(s,side):choose_oracle(s,side);
}
}
void init(const State&s,int me){bot::rng=0x7d2543a71b9ULL^s.wall_hash^(me*987654321ULL);for(int i=0;i<6;i++)bot::previous[i]=-1;}
Orders decide(const State&s,int me){Orders o=bot::choose(s,me);for(int i=0;i<6;i++)bot::previous[i]=s.tank[i].alive?s.tank[i].pos:-1;return o;}
