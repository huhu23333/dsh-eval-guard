// Final second-half bot. Rapid upgrade and siege, two flexible defenders.
// Supply assignment distinguishes travel from waiting, including before turn 10.
// A destructible cell can itself be a future firing position: seed its ray cost
// BEFORE charging for that wall, then charge demolition when walking into it.
// Enemy-occupied firing cells are excluded, and blockers add siege-ray cost.
// Distant shots at mobile tanks have exponentially declining expected value.
// Weighted path fields include the cost of opening brick/steel corridors.
// All tactical transitions use the authoritative simultaneous-action engine.
// Opponent response weights come from regret matching; risk tolerance follows
// its demonstrated upgrade progress. The final choice includes a second turn.
#include "tank.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>
using namespace tank;
namespace {
constexpr float INF=10000;
struct Context { const State* root; float attack[6][CELLS],supply[6][MAX_SUPPLY][CELLS],wall[2][CELLS],defend[6][CELLS]; int goal[6],threat[6]; };
static Context ctx;
static int priorGoal[6],visits[6][CELLS];
float walkCost(const State&s,int i,int c){int g=s.grid[c];float v=i%3==FAST?.56f:1.f;if(g==EMPTY)return v;if(g==BRICK||(g==STEEL&&s.tank[i].level>=2))return v+3.4f;return INF;}
void flood(const State&s,int i,float*dis){using Node=std::pair<float,int>;std::priority_queue<Node,std::vector<Node>,std::greater<Node>>q;for(int y=0;y<s.P.size;y++)for(int x=0;x<s.P.size;x++){int c=cell_of(x,y);if(dis[c]<INF)q.push({dis[c],c});}while(!q.empty()){auto[v,c]=q.top();q.pop();if(v!=dis[c])continue;float cost=walkCost(s,i,c);if(cost>=INF)continue;for(int d=0;d<4;d++){int n=c+DIR_DELTA[d];if(walkCost(s,i,n)>=INF)continue;float z=v+cost;if(z<dis[n]){dis[n]=z;q.push({z,n});}}}}
// Goals: -1 attacks the base; -2 intercepts an assigned threat; 0..2 seek supplies.
void prepare(const State&s){ctx.root=&s;for(int i=0;i<6;i++){float*a=ctx.attack[i];std::fill(a,a+CELLS,INF);int base=s.base_pos[1-i/3];for(int d=0;d<4;d++){float walls=0;int r=0;for(int c=base+DIR_DELTA[d];s.grid[c]!=OUTSIDE;c+=DIR_DELTA[d]){r++;int g=s.grid[c];if(g==BASE0||g==BASE1||(g==STEEL&&s.tank[i].level<2))break;int j=tank_at(s,c);
if((g==EMPTY||g==BRICK||g==STEEL)&&(j<0||j==i))a[c]=walls+.85f*r;
if(g==BRICK||g==STEEL)walls+=3.4f;
if(j>=0&&j!=i)walls+=j/3==i/3?2.f:5.f+1.5f*s.tank[j].hp;}}flood(s,i,a);for(int k=0;k<s.n_supply;k++){float*b=ctx.supply[i][k];std::fill(b,b+CELLS,INF);b[s.supply[k]]=0;flood(s,i,b);}}
int wait=s.turn<10?10-s.turn:20-(s.turn-10)%20;
for(int seat=0;seat<2;seat++){float best=-1e9;int bg[3]={-1,-1,-1};for(int a=-1;a<s.n_supply;a++)for(int b=-1;b<s.n_supply;b++)for(int c=-1;c<s.n_supply;c++){int gs[3]={a,b,c};if((a>=0&&a==b)||(a>=0&&a==c)||(b>=0&&b==c))continue;float v=0;bool valid=true;for(int t=0;t<3;t++){int i=3*seat+t,k=gs[t];const auto&tk=s.tank[i];int p=tk.alive?tk.pos:s.spawn[i];float av=18-1.35f*ctx.attack[i][p];if(k<0){v+=av*(t==0?2.5f:1.f);continue;}if(tk.level>=2||(t==0&&tk.level>=1)){valid=false;break;}float dis=ctx.supply[i][k][p];int w=(s.star_mask>>k&1)?0:wait;float sv=(tk.level==0?30.f:24.f)-2.4f*dis-(s.turn<10?.7f:2.4f)*std::max(0.f,float(w)-dis)-.6f*dis;if(priorGoal[i]==k)sv+=1.1f;if(!tk.alive)sv-=4;v+=sv*(t==0?2.5f:1.f);}if(valid&&v>best){best=v;for(int t=0;t<3;t++)bg[t]=gs[t];}}for(int t=0;t<3;t++)ctx.goal[3*seat+t]=bg[t];}
for(int seat=0;seat<2;seat++) {
 std::vector<std::pair<float,int>> threats;
 for(int i=3*seat;i<3*seat+3;i++)ctx.threat[i]=-1;
 for(int j=3*(1-seat);j<3*(1-seat)+3;j++)if(s.tank[j].alive) {
  int p=s.tank[j].pos,base=s.base_pos[seat],dy=std::abs(cell_y(p)-cell_y(base));float severity=8-dy;
  if(cell_x(p)==cell_x(base)||cell_y(p)==cell_y(base)) {
   int de=cell_x(p)==cell_x(base)?(base>p?STRIDE:-STRIDE):(base>p?1:-1);int walls=0;
   for(int c=p+de;c!=base;c+=de){if(s.grid[c]==BRICK)walls++;if(s.grid[c]==STEEL)walls+=5;}if(walls<=1)severity+=9;
  }if(dy<8&&severity>0)threats.push_back({severity,j});
 }std::sort(threats.rbegin(),threats.rend());
 for(int k=0;k<std::min(2,int(threats.size()));k++){
  int threat=threats[k].second;if(k>0&&threats[k].first<4)break;
  int besti=-1;float bestcost=1e9;
  for(int i=3*seat;i<3*seat+3;i++)if(ctx.goal[i]!=-2){
   float dis[CELLS];std::fill(dis,dis+CELLS,INF);int target=s.tank[threat].pos;
   for(int d=0;d<4;d++){float walls=0;for(int c=target+DIR_DELTA[d],r=1;r<=8;c+=DIR_DELTA[d],r++) {
    int g=s.grid[c];if(g==OUTSIDE||g==BASE0||g==BASE1||(g==STEEL&&s.tank[i].level<2))break;
    if(g==BRICK||g==STEEL)walls+=3.4f;if(g==EMPTY)dis[c]=walls+.6f*r;
   }}flood(s,i,dis);int p=s.tank[i].alive?s.tank[i].pos:s.spawn[i];float cost=dis[p];
   if(!s.tank[i].alive)cost+=respawn_in(s,i);if(ctx.attack[i][p]<2.5f&&s.tank[i].level>=1)cost+=16;if(i%3==FAST)cost+=20;
   if(cost<bestcost){bestcost=cost;besti=i;std::copy(dis,dis+CELLS,ctx.defend[i]);}
  }if(besti>=0&&bestcost<20){ctx.goal[besti]=-2;ctx.threat[besti]=threat;}
 }
}
for(int seat=0;seat<2;seat++){std::fill(ctx.wall[seat],ctx.wall[seat]+CELLS,0);for(int i=3*seat;i<3*seat+3;i++){int p=s.tank[i].alive?s.tank[i].pos:s.spawn[i];const float*dis=ctx.goal[i]==-2?ctx.defend[i]:(ctx.goal[i]<0?ctx.attack[i]:ctx.supply[i][ctx.goal[i]]);for(int d=0;d<4;d++){int n=p+DIR_DELTA[d];if(s.grid[n]==BRICK||(s.grid[n]==STEEL&&s.tank[i].level>=2))ctx.wall[seat][n]=std::max(ctx.wall[seat][n],std::max(0.f,dis[p]-dis[n])*.85f);}int base=ctx.goal[i]==-2?s.tank[ctx.threat[i]].pos:s.base_pos[1-seat];if(cell_x(p)==cell_x(base)||cell_y(p)==cell_y(base)){int delta=cell_x(p)==cell_x(base)?(base>p?STRIDE:-STRIDE):(base>p?1:-1);for(int c=p+delta;c!=base;c+=delta){if(s.grid[c]==OUTSIDE||(s.grid[c]==STEEL&&s.tank[i].level<2))break;if(s.grid[c]==BRICK||s.grid[c]==STEEL)ctx.wall[seat][c]=std::max(ctx.wall[seat][c],ctx.goal[i]==-2?12.f:4.5f);}}}}}
float position(const State&s,int i){const auto&t=s.tank[i];if(!t.alive)return -3.f*respawn_in(s,i);float v=18-1.35f*ctx.attack[i][t.pos];int k=ctx.goal[i];if(k==-2)v=60-4.2f*ctx.defend[i][t.pos];if(k>=0&&t.level<2&&t.level==ctx.root->tank[i].level)v=(t.level==0?30.f:24.f)-2.4f*ctx.supply[i][k][t.pos];v=std::max(-35.f,v);if(t.pos!=ctx.root->tank[i].pos)v-=std::min(2.f,visits[i][t.pos]*.12f);for(int j=3*(1-i/3);j<3*(1-i/3)+3;j++)if(!s.tank[j].alive&&s.spawn[j]==t.pos&&respawn_in(s,j)<=2)v-=280;return v;}
float evaluation(const State&s,int me){if(s.done)return s.winner<0?0:(s.winner==me?100000.f:-100000.f);float vals[2]={};for(int seat=0;seat<2;seat++){vals[seat]=70.f*s.base_hp[seat]+10.f*s.kills[seat]+1.5f*s.stars_eaten[seat];for(int i=3*seat;i<3*seat+3;i++){const Tank&t=s.tank[i];if(t.alive)vals[seat]+=35+48.f*t.level+18.f*t.hp+position(s,i);else vals[seat]+=position(s,i);}}
int occ[CELLS];std::fill(occ,occ+CELLS,-1);for(int i=0;i<6;i++)if(s.tank[i].alive)occ[s.tank[i].pos]=i;for(int b=0;b<s.n_bullets;b++){const auto&bl=s.bullet[b];int seat=bl.owner/3;for(int c=bl.pos+DIR_DELTA[bl.dir],r=1;r<=s.P.size+1;c+=DIR_DELTA[bl.dir],r++){int g=s.grid[c];if(g==OUTSIDE)break;if(g==BRICK||g==STEEL){if(g==BRICK||bl.steel)vals[seat]+=ctx.wall[seat][c]/(1+.12f*r);break;}if(g==BASE0||g==BASE1){int target=g-BASE0;vals[target]-=30.f/(1+.18f*r);break;}int j=occ[c];if(j>=0){float danger=(s.tank[j].hp<=1?25.f:12.f)*std::pow(.55f,float(r-1));int exits=0;for(int d=0;d<4;d++)if((d/2)!=(bl.dir/2)&&s.grid[c+DIR_DELTA[d]]==EMPTY&&occ[c+DIR_DELTA[d]]<0)exits++;if(exits==0)danger=std::max(danger,(s.tank[j].hp<=1?20.f:10.f)/(1+.25f*r));vals[j/3]-=danger;break;}}}
if(s.wall_hash!=ctx.root->wall_hash)for(int c=0;c<CELLS;c++)if((ctx.root->grid[c]==BRICK||ctx.root->grid[c]==STEEL)&&s.grid[c]==EMPTY)for(int seat=0;seat<2;seat++)vals[seat]+=ctx.wall[seat][c];return vals[me]-vals[1-me];}
struct Choice{Orders o;float value;};
std::vector<Choice>choices(const State&s,int seat,int limit,const Orders&other){std::vector<std::pair<float,Cmd>>ls[3];for(int t=0;t<3;t++){Cmd legal[N_CMDS];int n=legal_commands(s,3*seat+t,legal);for(int j=0;j<n;j++){Orders o;o.cmd[t]=legal[j];State z=s;step_as(z,seat,o,other);ls[t].push_back({evaluation(z,seat),legal[j]});}std::stable_sort(ls[t].begin(),ls[t].end(),[](auto a,auto b){return a.first>b.first;});if(ls[t].size()>7)ls[t].resize(7);}std::vector<Choice>out;for(auto a:ls[0])for(auto b:ls[1])for(auto c:ls[2]){Orders o;o.cmd[0]=a.second;o.cmd[1]=b.second;o.cmd[2]=c.second;State z=s;step_as(z,seat,o,other);out.push_back({o,evaluation(z,seat)});}std::stable_sort(out.begin(),out.end(),[](const Choice&a,const Choice&b){return a.value>b.value;});if(int(out.size())>limit)out.resize(limit);return out;}
Orders greedy(const State&s,int seat,const Orders&other){Orders o;for(int pass=0;pass<2;pass++)for(int t=0;t<3;t++){Cmd legal[N_CMDS];int n=legal_commands(s,3*seat+t,legal);float best=-1e9;Cmd bc=0;for(int k=0;k<n;k++){Orders a=o;a.cmd[t]=legal[k];State z=s;step_as(z,seat,a,other);float v=evaluation(z,seat);if(v>best){best=v;bc=legal[k];}}o.cmd[t]=bc;}return o;}
}
void init(const State&,int){std::fill(priorGoal,priorGoal+6,-1);for(auto&row:visits)std::fill(row,row+CELLS,0);}
Orders decide(const State&s,int me){prepare(s);float sophistication=std::clamp((s.stars_eaten[1-me]-1)*.5f,0.f,1.f); float rootRisk=.75f-.45f*sophistication, continuationRisk=.8f-.4f*sophistication;Orders idle;auto enemy=choices(s,1-me,24,idle);auto seeds=enemy;
for(int b=0;b<std::min(2,int(seeds.size()));b++)for(int t=0;t<3;t++) {
 Cmd legal[N_CMDS];int n=legal_commands(s,3*(1-me)+t,legal);
 for(int k=0;k<n;k++){Orders o=seeds[b].o;o.cmd[t]=legal[k];
  if(std::none_of(enemy.begin(),enemy.end(),[&](const Choice&z){return z.o==o;}))enemy.push_back({o,0});
 }
}
auto mine=choices(s,me,60,enemy[0].o);auto quiet=choices(s,me,10,idle);for(auto a:quiet)if(std::none_of(mine.begin(),mine.end(),[&](const Choice&b){return a.o==b.o;}))mine.push_back(a);struct Ranked{int index;float value;int response;};std::vector<Ranked>rank;
 int n=mine.size(),m=enemy.size();std::vector<float> matrix(n*m);
 for(int a=0;a<n;a++)for(int b=0;b<m;b++){State z=s;step_as(z,me,mine[a].o,enemy[b].o);matrix[a*m+b]=evaluation(z,me);}
 std::vector<float> r1(n),r2(m),p1(n),p2(m),avg2(m),u1(n),u2(m);
 for(int iter=0;iter<80;iter++) {
  float sum1=0,sum2=0;for(int a=0;a<n;a++)sum1+=std::max(0.f,r1[a]);for(int b=0;b<m;b++)sum2+=std::max(0.f,r2[b]);
  for(int a=0;a<n;a++)p1[a]=sum1>1e-5?std::max(0.f,r1[a])/sum1:1.f/n;
  for(int b=0;b<m;b++){p2[b]=sum2>1e-5?std::max(0.f,r2[b])/sum2:1.f/m;avg2[b]+=p2[b];}
  std::fill(u1.begin(),u1.end(),0);std::fill(u2.begin(),u2.end(),0);float v=0;
  for(int a=0;a<n;a++)for(int b=0;b<m;b++){float x=matrix[a*m+b];u1[a]+=p2[b]*x;u2[b]-=p1[a]*x;}
  for(int a=0;a<n;a++)v+=p1[a]*u1[a];for(int a=0;a<n;a++)r1[a]+=u1[a]-v;for(int b=0;b<m;b++)r2[b]+=u2[b]+v;
 }
 for(int a=0;a<n;a++){float worst=1e9,sum=0;int wi=0;for(int b=0;b<m;b++){float v=matrix[a*m+b];if(v<worst){worst=v;wi=b;}sum+=v*avg2[b]/80;}
 rank.push_back({a,rootRisk*worst+(1-rootRisk)*sum,wi});}
 std::stable_sort(rank.begin(),rank.end(),[](auto a,auto b){return a.value>b.value;});Orders result=mine[rank[0].index].o;float best=-1e9;for(int k=0;k<std::min(8,int(rank.size()));k++){if(budget_left()<12)break;auto r=rank[k];float worst=1e9,sum=0;int responses[3]={r.response,0,int(enemy.size())>1?1:0};for(int b:responses){if(budget_left()<6)return result;State z=s;step_as(z,me,mine[r.index].o,enemy[b].o);float v=evaluation(z,me);if(!z.done){Orders eo=greedy(z,1-me,idle),mo=greedy(z,me,eo);eo=greedy(z,1-me,mo);mo=greedy(z,me,eo);State q=z;step_as(q,me,mo,eo);v=.35f*v+.65f*evaluation(q,me);}worst=std::min(worst,v);sum+=v;}float value=.8f*(continuationRisk*worst+(1-continuationRisk)*sum/3)+.2f*r.value;if(value>best){best=value;result=mine[r.index].o;}}for(int i=0;i<6;i++){priorGoal[i]=ctx.goal[i];if(s.tank[i].alive)visits[i][s.tank[i].pos]++;}return result;}
