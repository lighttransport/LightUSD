// SPDX-License-Identifier: Apache-2.0
// Numeric standard-skinning diagnostic; does not render or bake the source.
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cmath>
#include "load-usd.hh"
#include "tydra/render-extract.hh"
int main(int argc,char** argv){
 if(argc!=3)return 2;
 namespace n=lightusd::next;namespace t=lightusd::tydra::next;
 n::Stage stage;std::string error,warn;
 if(!n::LoadUSDComposed(argv[1],&stage,&warn,&error)){std::cerr<<error;return 1;}
 char* end=nullptr;const double time=std::strtod(argv[2],&end);
 if(!end || end==argv[2] || *end || !std::isfinite(time))return 2;
 bool ok=true;
 std::cout<<std::setprecision(9);
 stage.Traverse([&](const n::UsdPrim& prim){
  if(prim.GetTypeName()!="Mesh")return true;
  t::ValueArrayRead<float> points;if(!t::ReadFloatArray(prim,"points",time,&points))return true;
  std::vector<float> skinned;
  if(!t::EvaluateSkinnedPoints(stage,prim,time,points,&skinned,&error)){std::cerr<<prim.GetPath().str()<<":"<<error<<"\n";ok=false;return true;}
  if(skinned.empty())return true;
  std::cout<<prim.GetPath().str()<<" "<<skinned.size();for(float v:skinned)std::cout<<" "<<v;std::cout<<"\n";return true;
 });return ok?0:1;
}
