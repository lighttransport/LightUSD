// SPDX-License-Identifier: Apache-2.0
// Shared scalar kernel source: compiled as C++ (CPU), CUDA and GLSL.
#ifndef PREVIEW_LOOP
#define PREVIEW_LOOP
#endif
#ifndef PREVIEW_PRECISE
#define PREVIEW_PRECISE
#endif
struct V { float x; float y; float z; };
FN V v(float x,float y,float z) { V a;a.x=x;a.y=y;a.z=z;return a; }
FN V add(V a,V b){return v(a.x+b.x,a.y+b.y,a.z+b.z);}
FN V sub(V a,V b){return v(a.x-b.x,a.y-b.y,a.z-b.z);}
FN V mul(V a,float b){return v(a.x*b,a.y*b,a.z*b);}
FN V had(V a,V b){return v(a.x*b.x,a.y*b.y,a.z*b.z);}
FN V crossv(V a,V b){return v(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
FN float dotv(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
FN V norm(V a){return mul(a,1.0f/sqrt(max(1e-30f,dotv(a,a))));}
FN float rf(DATA_ARG uint i){return BITS(inData[i]);}
FN V rv(DATA_ARG uint i){return v(rf(DATA_PASS i),rf(DATA_PASS i+1),rf(DATA_PASS i+2));}
FN float grid(DATA_ARG uint m,V p,uint channel){
  V q=mul(sub(p,rv(DATA_PASS m)),1.0f/rf(DATA_PASS m+3));
  int nx=int(inData[m+4]),ny=int(inData[m+5]),nz=int(inData[m+6]);
  int ix=int(floor(q.x)),iy=int(floor(q.y)),iz=int(floor(q.z));
  V f=v(q.x-float(ix),q.y-float(iy),q.z-float(iz));
  float result=0.0f;
  for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x){
    int xx=min(nx-1,max(0,ix+x)),yy=min(ny-1,max(0,iy+y)),zz=min(nz-1,max(0,iz+z));
    float w=(x==0?1.0f-f.x:f.x)*(y==0?1.0f-f.y:f.y)*(z==0?1.0f-f.z:f.z);
    result+=w*rf(DATA_PASS inData[m+7]+uint((zz*ny+yy)*nx+xx)*4+channel);
  }
  return result;
}
FN float axisValue(V a,int k){return k==0?a.x:(k==1?a.y:a.z);}
FN float boxInterval(V lo,V hi,V o,V d,float begin,float end,uint exitOnly){
  float a=begin,b=end;
  for(int k=0;k<3;++k){
    float x=axisValue(o,k),vdir=axisValue(d,k),mn=axisValue(lo,k),mx=axisValue(hi,k);
    if(abs(vdir)<1e-20f){if(x<mn||x>mx)return -1.0f;}
    else{float x0=(mn-x)/vdir,x1=(mx-x)/vdir;a=max(a,min(x0,x1));b=min(b,max(x0,x1));}
  }
  return b>a?(exitOnly!=0u?b:a):-1.0f;
}
FN float interval(DATA_ARG uint m,V o,V d,float begin,float end,uint exitOnly){
  float cell=rf(DATA_PASS m+3u);V lo=rv(DATA_PASS m);
  V hi=add(lo,v(float(inData[m+4u]-1u)*cell,float(inData[m+5u]-1u)*cell,float(inData[m+6u]-1u)*cell));
  return boxInterval(lo,hi,o,d,begin,end,exitOnly);
}
FN uint fieldAtLinear(DATA_ARG V p,uint group,uint matching){
  uint best=0u;float size=1e30f;
  for(uint b=0u;b<inData[3];++b){
    uint m=64u+b*16u;if(matching!=0u && inData[m+9u]!=group)continue;float cell=rf(DATA_PASS m+3u);
    V lo=rv(DATA_PASS m),hi=add(lo,v(float(inData[m+4u]-1u)*cell,float(inData[m+5u]-1u)*cell,float(inData[m+6u]-1u)*cell));
    if(cell<size&&p.x>=lo.x&&p.y>=lo.y&&p.z>=lo.z&&p.x<=hi.x&&p.y<=hi.y&&p.z<=hi.z){size=cell;best=m;}
  }
  return best;
}
FN uint fieldAtMatch(DATA_ARG V p,uint group,uint matching){
  if(inData[40]==0u)return fieldAtLinear(DATA_PASS p,group,matching);
  uint index=0u,best=0u;float size=1e30f;
  while(index<inData[41]){
    uint node=inData[40]+index*8u;V lo=rv(DATA_PASS node),hi=rv(DATA_PASS node+3u);
    if(p.x<lo.x||p.y<lo.y||p.z<lo.z||p.x>hi.x||p.y>hi.y||p.z>hi.z){index=inData[node+7u];continue;}
    uint left=inData[node+6u];
    if((left&0x80000000u)!=0u){
      uint m=left&0x7fffffffu;
      if(matching==0u || inData[m+9u]==group){float cell=rf(DATA_PASS m+3u);if(cell<size || (cell==size && (best==0u||m<best))){size=cell;best=m;}}
      index=inData[node+7u];
    }else ++index;
  }
  return best;
}
FN uint fieldAt(DATA_ARG V p){return fieldAtMatch(DATA_PASS p,0u,0u);}
FN float density(DATA_ARG uint m,V p){
  if(m==0u)return 0.0f;
  float f=grid(DATA_PASS m,p,0u),cell=rf(DATA_PASS m+3u);
  return rf(DATA_PASS 28u)*(inData[m+8u]!=0u?max(0.0f,f):max(0.0f,min(1.0f,.5f-f/cell))/cell);
}
struct MediumState { float sigma; V color; float cell; uint field; uint solid; };
FN MediumState emptyMedium(){MediumState a;a.sigma=0.0f;a.color=v(0.0f,0.0f,0.0f);a.cell=1e30f;a.field=0u;a.solid=0u;return a;}
FN MediumState addMedium(DATA_ARG MediumState a,uint m,V p,uint surfaces){
  if(inData[m+10u]==0u && fieldAtMatch(DATA_PASS p,inData[m+9u],1u)!=m)return a;
  float cell=rf(DATA_PASS m+3u),f=grid(DATA_PASS m,p,0u);
  if(cell<a.cell || (cell==a.cell && (a.field==0u||m<a.field))){a.cell=cell;a.field=m;}
  if(inData[m+8u]==0u && f<-.01f*cell)a.solid=1u;
  float sigma=0.0f;
  if(inData[m+8u]!=0u)sigma=max(0.0f,f)*rf(DATA_PASS 28u);
  else if(surfaces!=0u)sigma=max(0.0f,min(1.0f,.5f-f/cell))*rf(DATA_PASS 28u)/cell;
  if(sigma>0.0f){
    V color=v(max(0.0f,min(1.0f,grid(DATA_PASS m,p,1u))),max(0.0f,min(1.0f,grid(DATA_PASS m,p,2u))),max(0.0f,min(1.0f,grid(DATA_PASS m,p,3u))));
    a.sigma+=sigma;a.color=add(a.color,mul(color,sigma));
  }
  return a;
}
FN MediumState mediumAtLinear(DATA_ARG V p,uint surfaces){
  MediumState a=emptyMedium();
  for(uint b=0u;b<inData[3];++b){uint m=64u+b*16u;float cell=rf(DATA_PASS m+3u);V lo=rv(DATA_PASS m);
    V hi=add(lo,v(float(inData[m+4u]-1u)*cell,float(inData[m+5u]-1u)*cell,float(inData[m+6u]-1u)*cell));
    if(p.x>=lo.x&&p.y>=lo.y&&p.z>=lo.z&&p.x<=hi.x&&p.y<=hi.y&&p.z<=hi.z)a=addMedium(DATA_PASS a,m,p,surfaces);
  }
  return a;
}
FN MediumState mediumAt(DATA_ARG V p,uint surfaces){
  if(inData[40]==0u)return mediumAtLinear(DATA_PASS p,surfaces);
  MediumState a=emptyMedium();uint index=0u;
  while(index<inData[41]){uint node=inData[40]+index*8u;V lo=rv(DATA_PASS node),hi=rv(DATA_PASS node+3u);
    if(p.x<lo.x||p.y<lo.y||p.z<lo.z||p.x>hi.x||p.y>hi.y||p.z>hi.z){index=inData[node+7u];continue;}
    uint left=inData[node+6u];
    if((left&0x80000000u)!=0u){a=addMedium(DATA_PASS a,left&0x7fffffffu,p,surfaces);index=inData[node+7u];}
    else ++index;
  }
  return a;
}
struct BoundSegment {float start;float end;float cell;float nextEntry;};
FN BoundSegment addSegment(DATA_ARG BoundSegment seg,uint m,V o,V d,float t,float farT){
  float a=interval(DATA_PASS m,o,d,t,farT,0u);if(a<0.0f)return seg;
  float z=interval(DATA_PASS m,o,d,t,farT,1u);
  if(a<=t){seg.start=t;seg.end=min(seg.end,z);seg.cell=min(seg.cell,rf(DATA_PASS m+3u));}
  else seg.nextEntry=min(seg.nextEntry,a);
  return seg;
}
FN BoundSegment raySegmentLinear(DATA_ARG V o,V d,float t,float farT){
  BoundSegment seg;seg.start=farT;seg.end=farT;seg.cell=1e30f;seg.nextEntry=farT;
  for(uint b=0u;b<inData[3];++b)seg=addSegment(DATA_PASS seg,64u+b*16u,o,d,t,farT);
  if(seg.start==t)seg.end=min(seg.end,seg.nextEntry);else seg.start=seg.nextEntry;
  return seg;
}
FN BoundSegment raySegment(DATA_ARG V o,V d,float t,float farT){
  if(inData[40]==0u)return raySegmentLinear(DATA_PASS o,d,t,farT);
  BoundSegment seg;seg.start=farT;seg.end=farT;seg.cell=1e30f;seg.nextEntry=farT;
  uint index=0u;
  while(index<inData[41]){uint node=inData[40]+index*8u;
    if(boxInterval(rv(DATA_PASS node),rv(DATA_PASS node+3u),o,d,t,farT,0u)<0.0f){index=inData[node+7u];continue;}
    uint left=inData[node+6u];
    if((left&0x80000000u)!=0u){seg=addSegment(DATA_PASS seg,left&0x7fffffffu,o,d,t,farT);index=inData[node+7u];}
    else ++index;
  }
  if(seg.start==t)seg.end=min(seg.end,seg.nextEntry);else seg.start=seg.nextEntry;
  return seg;
}
struct MediumIntegral {float dt;float tau;V color;MediumState middle;uint solid;uint complete;};
FN MediumIntegral integrateMedium(DATA_ARG V o,V d,float t,float proposed,uint surfaces){
  MediumIntegral value;value.dt=proposed;value.tau=0.0f;value.color=v(0.0f,0.0f,0.0f);value.middle=emptyMedium();value.solid=0u;value.complete=1u;
  float tolerance=rf(DATA_PASS 43u);
  PREVIEW_LOOP for(int refine=0;refine<16;++refine){
    value.middle=mediumAt(DATA_PASS add(o,mul(d,t+value.dt*.5f)),surfaces);
    MediumState a=mediumAt(DATA_PASS add(o,mul(d,t+value.dt*.211324865405187f)),surfaces);
    MediumState b=mediumAt(DATA_PASS add(o,mul(d,t+value.dt*.788675134594813f)),surfaces);
    value.tau=(a.sigma+b.sigma)*(.5f*value.dt);value.color=mul(add(a.color,b.color),.5f*value.dt);
    value.solid=value.middle.solid|a.solid|b.solid;
    V difference=sub(value.color,mul(value.middle.color,value.dt));
    float error=max(abs(value.tau-value.middle.sigma*value.dt),max(abs(difference.x),max(abs(difference.y),abs(difference.z))));
    if(tolerance<=0.0f || error<=tolerance*max(.0001f,value.tau))return value;
    if(refine==15 || value.dt<=proposed*.0001f){value.complete=0u;return value;}
    value.dt*=.5f;
  }
  return value;
}
FN float oneMinusExp(float x){return x<.001f?x*(1.0f-.5f*x+x*x/6.0f):1.0f-exp(-min(80.0f,x));}
FN float visibility(DATA_ARG V p,V direction,float distance,uint medium){
  if(inData[34]==0u)return 1.0f;
  PREVIEW_PRECISE float optical=0.0f,compensation=0.0f,t=.0001f;
  uint limit=inData[39]!=0u?inData[39]:1024u;
  for(uint step=0u;step<limit;++step){
    if(t>=distance)return exp(-min(80.0f,optical));
    if(rf(DATA_PASS 46u)>0.0f && exp(-min(80.0f,optical))<rf(DATA_PASS 46u))return 0.0f;
    BoundSegment seg=raySegment(DATA_PASS p,direction,t,distance);
    if(seg.start>=distance)return exp(-min(80.0f,optical));
    if(seg.start>t){t=seg.start;continue;}
    float scale=rf(DATA_PASS 42u);if(scale<=0.0f)scale=.5f;
    MediumIntegral value=integrateMedium(DATA_PASS p,direction,t,min(seg.cell*scale,seg.end-t),medium);
    if(value.complete==0u)return BITS(0x7fc00000u);
    if(medium==0u && value.solid!=0u)return 0.0f;
    if(value.dt<=0.0f || t+value.dt<=t)return BITS(0x7fc00000u);
    PREVIEW_PRECISE float delta=value.tau-compensation,next=optical+delta;compensation=(next-optical)-delta;optical=next;t+=value.dt;
  }
  BoundSegment tail=raySegment(DATA_PASS p,direction,t,distance);
  return tail.start>=distance?exp(-min(80.0f,optical)):BITS(0x7fc00000u);
}
// Light/shadow linking. Preview receivers are media prims; a zero mask links
// everything and receiver 0 (no identified prim, e.g. an isosurface) is
// unfiltered.
FN uint linkTest(DATA_ARG uint l,uint lane,uint receiver){
  uint mask=inData[l+lane];
  if(mask==0u||receiver==0u)return 1u;
  return (inData[mask+(receiver>>5u)]>>(receiver&31u))&1u;
}
FN V texel(DATA_ARG uint l,float u,float t){
  if(inData[l+16u]==0u)return v(1.0f,1.0f,1.0f);
  int w=int(inData[l+17u]),h=int(inData[l+18u]);
  bool repeat=inData[l+19u]==1u;
  u=repeat?u-floor(u):max(0.0f,min(1.0f,u));t=max(0.0f,min(1.0f,t));
  float x=u*float(w)-.5f,y=t*float(h)-.5f;
  int ix=int(floor(x)),iy=int(floor(y));float fx=x-float(ix),fy=y-float(iy);
  V result=v(0.0f,0.0f,0.0f);
  for(int j=0;j<2;++j)for(int i=0;i<2;++i){
    int xx=repeat?((ix+i)%w+w)%w:min(w-1,max(0,ix+i)),yy=min(h-1,max(0,iy+j));
    float weight=(i==0?1.0f-fx:fx)*(j==0?1.0f-fy:fy);
    result=add(result,mul(rv(DATA_PASS inData[l+16u]+uint(yy*w+xx)*3u),weight));
  }
  return result;
}
FN V domeTexture(DATA_ARG uint l,V direction){
  V q=v(dotv(direction,rv(DATA_PASS l+20u)),dotv(direction,rv(DATA_PASS l+24u)),dotv(direction,rv(DATA_PASS l+28u)));
  q=norm(q);
  // Azimuth is undefined at a pole; choose one convention on every backend.
  float azimuth=q.x*q.x+q.z*q.z<1e-20f?0.0f:atan2(q.x,-q.z);
  float u=.5f+azimuth*.159154943091895f,t=acos(max(-1.0f,min(1.0f,q.y)))*.318309886183791f;
  if(inData[l+19u]==2u){
    float radius=acos(max(-1.0f,min(1.0f,-q.z)))*.159154943091895f,r=sqrt(max(1e-20f,q.x*q.x+q.y*q.y));
    u=.5f+radius*q.x/r;t=.5f-radius*q.y/r;
  }
  if(inData[l+19u]==3u){
    float r=2.0f*sqrt(max(1e-20f,q.x*q.x+q.y*q.y+(1.0f-q.z)*(1.0f-q.z)));
    u=.5f+q.x/r;t=.5f-q.y/r;
  }
  return texel(DATA_PASS l,u,t);
}
FN float randomValue(uint seed,uint dimension){
  uint x=seed+dimension*0x9e3779b9u+0x85ebca6bu;x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
  return (float(x>>8)+.5f)*.000000059604644775390625f;
}
// Normalized prefix table stores N+1 edges. Zero-energy texels have no mass.
FN uint chooseEmitter(DATA_ARG uint l,float u){
  uint cdf=inData[l+31u],lo=0u,hi=inData[l+17u]*inData[l+18u]-1u;
  u=min(.99999994f,u);
  while(lo<hi){uint mid=lo+(hi-lo)/2u;if(u>=rf(DATA_PASS cdf+mid+1u))lo=mid+1u;else hi=mid;}
  return lo;
}
FN V scatterDirection(V direction,float g,float u,float t){
  float cosine=1.0f-2.0f*u;
  if(abs(g)>.001f){float q=(1.0f-g*g)/(1.0f-g+2.0f*g*u);cosine=(1.0f+g*g-q*q)/(2.0f*g);}
  cosine=max(-1.0f,min(1.0f,cosine));float sine=sqrt(max(0.0f,1.0f-cosine*cosine)),phi=6.283185307179586f*t;
  V axis=abs(direction.z)<.99f?v(0.0f,0.0f,1.0f):v(0.0f,1.0f,0.0f),right=norm(crossv(axis,direction)),up=crossv(direction,right);
  return norm(add(mul(direction,cosine),add(mul(right,sine*cos(phi)),mul(up,sine*sin(phi)))));
}
FN float phase(float cosine,float g){return (1.0f-g*g)/pow(max(.001f,1.0f+g*g-2.0f*g*cosine),1.5f);}
FN V illumination(DATA_ARG V p,V normal,V ray,float cell,uint medium,uint seed,uint receiver){
  if(inData[33]==0u||inData[25]==0u){
    float shade=medium!=0u?1.0f:.2f+.8f*abs(dotv(normal,ray));
    return v(shade,shade,shade);
  }
  V sum=v(0.0f,0.0f,0.0f);
  if(dotv(normal,ray)>0.0f)normal=mul(normal,-1.0f);
  for(uint i=0u;i<inData[25];++i){
    uint l=inData[26]+i*40u,type=inData[l+3u];
    if(linkTest(DATA_PASS l,33u,receiver)==0u)continue;
    V energy=rv(DATA_PASS l+8u);
    uint lightSeed=seed+i*0x9e3779b9u;
    if(type==5u && inData[l+31u]!=0u){
      uint taps=max(1u,inData[38]),w=inData[l+17u],h=inData[l+18u],cdf=inData[l+31u];
      for(uint tap=0u;tap<taps;++tap){
        uint tex=chooseEmitter(DATA_PASS l,randomValue(lightSeed,tap*3u+101u)),y=tex/w;
        float probability=rf(DATA_PASS cdf+tex+1u)-rf(DATA_PASS cdf+tex);
        float u=(float(tex%w)+randomValue(lightSeed,tap*3u+103u))/float(w);
        float t=(float(y)+randomValue(lightSeed,tap*3u+102u))/float(h);
        V local;float integral;
        if(inData[l+19u]==1u){
          float a=cos(3.141592653589793f*float(y)/float(h)),b=cos(3.141592653589793f*float(y+1u)/float(h));
          float cy=a+(b-a)*randomValue(lightSeed,tap*3u+102u),sy=sqrt(max(0.0f,1.0f-cy*cy));
          float phi=6.283185307179586f*(u-.5f);
          local=v(sin(phi)*sy,cy,-cos(phi)*sy);
          integral=(a-b)*6.283185307179586f/float(w);
        }else{
          float x=u-.5f,z=.5f-t,r=sqrt(x*x+z*z);
          if(r>=.5f)continue;
          float theta=inData[l+19u]==2u?r*6.283185307179586f:2.0f*asin(min(1.0f,2.0f*r));
          float sine=sin(theta);
          local=r>1e-8f?v(sine*x/r,sine*z/r,-cos(theta)):v(0.0f,0.0f,-1.0f);
          integral=(inData[l+19u]==3u?16.0f:(r<1e-8f?39.47841760435743f:6.283185307179586f*sine/r))/float(w*h);
        }
        V dir=norm(add(mul(rv(DATA_PASS l+20u),local.x),add(mul(rv(DATA_PASS l+24u),local.y),mul(rv(DATA_PASS l+28u),local.z))));
      float weight=medium!=0u?phase(dotv(dir,ray),rf(DATA_PASS 31u))*.0795774715459477f:max(0.0f,dotv(normal,dir))*.318309886183791f;
        weight*=integral/max(1e-30f,probability);
        if(weight<=0.0f)continue;
        V start=medium!=0u?p:add(p,mul(normal,cell*1.5f));
        float shadow=inData[l+12u]!=0u&&linkTest(DATA_PASS l,34u,receiver)!=0u?visibility(DATA_PASS start,dir,1e20f,medium):1.0f;
        V emission=rv(DATA_PASS inData[l+16u]+tex*3u);
        sum=add(sum,mul(had(energy,emission),weight*shadow/float(taps)));
      }
      continue;
    }
    if(type==5u){
      uint taps=max(1u,inData[38]);
      for(uint tap=0u;tap<taps;++tap){
        float u=randomValue(lightSeed,tap*2u+101u),t=randomValue(lightSeed,tap*2u+102u);V dir;
        if(medium!=0u)dir=scatterDirection(ray,rf(DATA_PASS 31u),u,t);
        else{
          float radius=sqrt(u),phi=t*6.283185307179586f;
          V axis=abs(normal.z)<.99f?v(0.0f,0.0f,1.0f):v(0.0f,1.0f,0.0f),right=norm(crossv(axis,normal)),up=crossv(normal,right);
          dir=norm(add(mul(normal,sqrt(1.0f-u)),add(mul(right,radius*cos(phi)),mul(up,radius*sin(phi)))));
        }
        V start=medium!=0u?p:add(p,mul(normal,cell*1.5f));
        float shadow=inData[l+12u]!=0u&&linkTest(DATA_PASS l,34u,receiver)!=0u?visibility(DATA_PASS start,dir,1e20f,medium):1.0f;
        V texture=inData[l+16u]==0u?v(1.0f,1.0f,1.0f):domeTexture(DATA_PASS l,dir);
        sum=add(sum,mul(had(energy,texture),shadow/float(taps)));
      }
      continue;
    }
    bool importance=type==3u && inData[l+31u]!=0u;
    bool rectangle=type==3u && rf(DATA_PASS l+23u)>0.0f && rf(DATA_PASS l+27u)>0.0f;
    bool disk=type==4u && rf(DATA_PASS l+23u)>0.0f && rf(DATA_PASS l+27u)>0.0f;
    bool sphere=(type==6u||type==2u) && rf(DATA_PASS l+23u)>0.0f && rf(DATA_PASS l+27u)>0.0f && rf(DATA_PASS l+7u)>0.0f;
    bool cylinder=type==7u && rf(DATA_PASS l+23u)>0.0f && rf(DATA_PASS l+27u)>0.0f && rf(DATA_PASS l+7u)>0.0f;
    bool finiteArea=rectangle||disk||sphere||cylinder;
    float radius=rf(DATA_PASS l+7u);V centerDelta=sub(rv(DATA_PASS l),p);float centerDistance=sqrt(max(1e-30f,dotv(centerDelta,centerDelta)));
    bool sphericalCone=sphere && abs(rf(DATA_PASS l+23u)-radius)<radius*1e-5f && abs(rf(DATA_PASS l+27u)-radius)<radius*1e-5f &&
      abs(dotv(rv(DATA_PASS l+20u),rv(DATA_PASS l+24u)))<1e-5f && abs(dotv(rv(DATA_PASS l+20u),rv(DATA_PASS l+28u)))<1e-5f &&
      abs(dotv(rv(DATA_PASS l+24u),rv(DATA_PASS l+28u)))<1e-5f && centerDistance>radius*1.00001f;
    bool cone=type==1u && rf(DATA_PASS l+7u)>0.0f;
    uint taps=finiteArea||importance||cone?max(1u,inData[38]):1u;
    if((rectangle||disk||sphericalCone) && rf(DATA_PASS l+14u)<.001f*centerDistance*centerDistance)taps=1u;
    for(uint tap=0u;tap<taps;++tap){
      V position=rv(DATA_PASS l),emission=energy,emitterNormal=norm(rv(DATA_PASS l+4u));
      float sampleArea=rf(DATA_PASS l+14u),solidAngle=0.0f,sampledDistance=0.0f;V sampledDirection=v(0.0f,0.0f,0.0f);
      float u=randomValue(lightSeed,tap*3u+102u),t=randomValue(lightSeed,tap*3u+103u);
      if(importance){
        uint tex=chooseEmitter(DATA_PASS l,randomValue(lightSeed,tap*3u+101u)),w=inData[l+17u],h=inData[l+18u],cdf=inData[l+31u];
        float probability=rf(DATA_PASS cdf+tex+1u)-rf(DATA_PASS cdf+tex);
        u=(float(tex%w)+u)/float(w);t=1.0f-(float(tex/w)+t)/float(h);
        emission=mul(had(emission,rv(DATA_PASS inData[l+16u]+tex*3u)),1.0f/(float(w*h)*max(1e-30f,probability)));
      }
      if(rectangle || importance){
        V x=rv(DATA_PASS l+20u),y=rv(DATA_PASS l+24u);
        position=add(position,add(mul(x,(u-.5f)*rf(DATA_PASS l+23u)),mul(y,(t-.5f)*rf(DATA_PASS l+27u))));
        V perpendicular=norm(crossv(x,y));if(dotv(perpendicular,emitterNormal)<0.0f)perpendicular=mul(perpendicular,-1.0f);emitterNormal=perpendicular;
        if(!importance)emission=had(emission,texel(DATA_PASS l,u,1.0f-t));
      }else if(disk){
        float radius=sqrt(u),phi=t*6.283185307179586f;V x=rv(DATA_PASS l+20u),y=rv(DATA_PASS l+24u);
        position=add(position,add(mul(x,radius*cos(phi)*rf(DATA_PASS l+23u)),mul(y,radius*sin(phi)*rf(DATA_PASS l+27u))));
        V perpendicular=norm(crossv(x,y));if(dotv(perpendicular,emitterNormal)<0.0f)perpendicular=mul(perpendicular,-1.0f);emitterNormal=perpendicular;
      }else if(sphericalCone){
        V axis=mul(centerDelta,1.0f/centerDistance),reference=abs(axis.z)<.99f?v(0.0f,0.0f,1.0f):v(0.0f,1.0f,0.0f),right=norm(crossv(reference,axis)),up=crossv(axis,right);
        float ratio=radius/centerDistance,cosMax=sqrt(max(0.0f,1.0f-ratio*ratio)),oneMinusCos=ratio*ratio/(1.0f+cosMax);
        float cosine=1.0f-u*oneMinusCos,sine=sqrt(max(0.0f,u*oneMinusCos*(2.0f-u*oneMinusCos))),phi=t*6.283185307179586f;
        sampledDirection=norm(add(mul(axis,cosine),add(mul(right,sine*cos(phi)),mul(up,sine*sin(phi)))));
        sampledDistance=centerDistance*cosine-sqrt(max(0.0f,radius*radius-centerDistance*centerDistance*sine*sine));
        position=add(p,mul(sampledDirection,sampledDistance));solidAngle=6.283185307179586f*oneMinusCos;
      }else if(sphere){
        float z=1.0f-2.0f*u,phi=t*6.283185307179586f,sine=sqrt(max(0.0f,1.0f-z*z));V q=v(sine*cos(phi),sine*sin(phi),z);
        V x=mul(rv(DATA_PASS l+20u),rf(DATA_PASS l+23u)),y=mul(rv(DATA_PASS l+24u),rf(DATA_PASS l+27u)),zaxis=mul(rv(DATA_PASS l+28u),rf(DATA_PASS l+7u));
        position=add(position,add(mul(x,q.x),add(mul(y,q.y),mul(zaxis,q.z))));
        V jac=add(mul(crossv(y,zaxis),q.x),add(mul(crossv(zaxis,x),q.y),mul(crossv(x,y),q.z)));
        sampleArea=12.566370614359172f*sqrt(max(0.0f,dotv(jac,jac)));emitterNormal=norm(jac);
        if(dotv(x,crossv(y,zaxis))<0.0f)emitterNormal=mul(emitterNormal,-1.0f);
      }else if(cylinder){
        float phi=t*6.283185307179586f;V x=mul(rv(DATA_PASS l+20u),rf(DATA_PASS l+23u)),y=mul(rv(DATA_PASS l+24u),rf(DATA_PASS l+27u)),z=mul(rv(DATA_PASS l+28u),rf(DATA_PASS l+7u));
        V radial=add(mul(y,cos(phi)),mul(z,sin(phi))),tangent=add(mul(y,-sin(phi)),mul(z,cos(phi)));
        position=add(position,add(mul(x,u-.5f),radial));V jac=crossv(tangent,x);sampleArea=6.283185307179586f*sqrt(max(0.0f,dotv(jac,jac)));emitterNormal=norm(jac);
        if(dotv(emitterNormal,radial)<0.0f)emitterNormal=mul(emitterNormal,-1.0f);
      }
      V delta=sub(position,p);
      float dist=sqrt(max(1e-20f,dotv(delta,delta))),area=rf(DATA_PASS l+14u);
      V dir=mul(delta,1.0f/dist);float falloff=1.0f;
      if(solidAngle>0.0f){dir=sampledDirection;dist=sampledDistance;}
      if(type==1u){
        dir=mul(norm(rv(DATA_PASS l+4u)),-1.0f);dist=1e20f;
        if(cone){
          float cosine=1.0f-u*(1.0f-cos(rf(DATA_PASS l+7u))),sine=sqrt(max(0.0f,1.0f-cosine*cosine)),phi=t*6.283185307179586f;
          V axis=abs(dir.z)<.99f?v(0.0f,0.0f,1.0f):v(0.0f,1.0f,0.0f),right=norm(crossv(axis,dir)),up=crossv(dir,right);
          dir=norm(add(mul(dir,cosine),add(mul(right,sine*cos(phi)),mul(up,sine*sin(phi)))));
        }
      }
      else {
        falloff=(inData[l+15u]!=0u?1.0f:area)/max(1e-12f,dist*dist);
        if(finiteArea || importance)falloff=sampleArea*(inData[l+15u]!=0u?1.0f/max(1e-12f,area):1.0f)*max(0.0f,dotv(emitterNormal,mul(dir,-1.0f)))/max(1e-12f,dist*dist);
        else if(type==3u||type==4u)falloff*=max(0.0f,dotv(emitterNormal,mul(dir,-1.0f)));
        if(solidAngle>0.0f)falloff=solidAngle*(inData[l+15u]!=0u?1.0f/max(1e-12f,area):1.0f);
        if(type==2u){
          float cone=rf(DATA_PASS l+11u),cosine=dotv(norm(rv(DATA_PASS l+4u)),mul(dir,-1.0f));
          falloff*=max(0.0f,min(1.0f,(cosine-cone)/max(.001f,(1.0f-cone)*rf(DATA_PASS l+13u))));
        }
      }
      // Photometric shaping: resampled 32x16 candela table in the light's
      // local frame, bilinear in vertical angle and azimuth (wrapped).
      uint ies=inData[l+32u];
      if(ies!=0u){
        V lx=rv(DATA_PASS ies),ly=rv(DATA_PASS ies+3u),lz=rv(DATA_PASS ies+6u);
        V shaped=v(dotv(dir,lx),dotv(dir,ly),dotv(dir,lz));
        float theta=acos(max(-1.0f,min(1.0f,-shaped.z)));
        float phi=atan2(shaped.y,shaped.x);if(phi<0.0f)phi+=6.283185307179586f;
        float fv=theta*.3183098861837907f*31.0f;
        int v0=int(floor(fv)),v1=min(v0+1,31);float tv=fv-float(v0);
        float fh=phi*2.5464790894703255f;
        int h0=int(floor(fh));if(h0>15)h0=0;int h1=(h0+1)&15;
        float th=fh-floor(fh);
        uint row0=ies+9u+uint(v0)*16u,row1=ies+9u+uint(v1)*16u;
        float a0=rf(DATA_PASS row0+uint(h0)),a1=rf(DATA_PASS row0+uint(h1));
        float b0=rf(DATA_PASS row1+uint(h0)),b1=rf(DATA_PASS row1+uint(h1));
        float top=a0+(a1-a0)*th,bottom=b0+(b1-b0)*th;
        emission=mul(emission,top+(bottom-top)*tv);
      }
      float weight=medium!=0u?phase(dotv(dir,ray),rf(DATA_PASS 31u))*.0795774715459477f:max(0.0f,dotv(normal,dir))*.318309886183791f;
      if(weight<=0.0f||falloff<=0.0f)continue;
      V start=medium!=0u?p:add(p,mul(normal,cell*1.5f));
      float shadow=inData[l+12u]!=0u&&linkTest(DATA_PASS l,34u,receiver)!=0u?visibility(DATA_PASS start,dir,dist,medium):1.0f;
      sum=add(sum,mul(emission,weight*falloff*shadow/float(taps)));
    }
  }
  return sum;
}
struct Collision { V point;MediumState medium;uint complete;};
FN Collision collision(DATA_ARG V o,V d,float nearT,float farT,float optical){
  Collision event;event.point=o;event.medium=emptyMedium();event.complete=1u;float t=nearT;
  for(uint step=0u;step<inData[32];++step){
    if(t>=farT)return event;
    BoundSegment seg=raySegment(DATA_PASS o,d,t,farT);
    if(seg.start>=farT)return event;
    if(seg.start>t){t=seg.start;continue;}
    float scale=rf(DATA_PASS 42u);if(scale<=0.0f)scale=.5f;
    MediumIntegral value=integrateMedium(DATA_PASS o,d,t,min(seg.cell*scale,seg.end-t),1u);
    if(value.complete==0u || value.dt<=0.0f || t+value.dt<=t){event.complete=0u;return event;}
    if(optical<value.tau){
      float a=0.0f,b=value.dt;
      PREVIEW_LOOP for(int k=0;k<20;++k){float mid=(a+b)*.5f;
        MediumState q0=mediumAt(DATA_PASS add(o,mul(d,t+mid*.211324865405187f)),1u);
        MediumState q1=mediumAt(DATA_PASS add(o,mul(d,t+mid*.788675134594813f)),1u);
        if((q0.sigma+q1.sigma)*mid*.5f<optical)a=mid;else b=mid;
      }
      event.point=add(o,mul(d,t+(a+b)*.5f));event.medium=mediumAt(DATA_PASS event.point,1u);return event;
    }
    optical-=value.tau;t+=value.dt;
  }
  event.complete=raySegment(DATA_PASS o,d,t,farT).start>=farT?1u:0u;return event;
}
struct RayResult {V color;uint complete;};
FN RayResult multipleRay(DATA_ARG uint pixel,V origin,V direction,float nearT,float farT,V background){
  RayResult result;result.color=v(0.0f,0.0f,0.0f);result.complete=1u;
  for(uint path=0u;path<inData[36];++path){
    uint seed=pixel*0x9e3779b9u+path*0x85ebca6bu+inData[37];
    V o=origin,d=direction,through=v(1.0f,1.0f,1.0f);
    for(uint bounce=0u;bounce<inData[35];++bounce){
      float optical=-log(max(1e-8f,1.0f-randomValue(seed,bounce*4u)));
      Collision event=collision(DATA_PASS o,d,bounce==0u?nearT:0.0f,bounce==0u?farT:1e30f,optical);
      result.complete&=event.complete;
      if(event.medium.field==0u){if(bounce==0u)result.color=add(result.color,had(through,background));break;}
      MediumState mix=event.medium;V p=event.point,albedo=mul(mix.color,1.0f/max(1e-30f,mix.sigma));
      result.color=add(result.color,mul(had(through,albedo),rf(DATA_PASS 30u)));
      through=mul(had(through,albedo),rf(DATA_PASS 29u));
      if(max(through.x,max(through.y,through.z))<=0.0f)break;
      result.color=add(result.color,had(through,illumination(DATA_PASS p,v(0.0f,0.0f,0.0f),d,mix.cell,1u,seed+bounce*0x7feb352du,mix.field!=0u?inData[mix.field+11u]:0u)));
      if(bounce>=4u){
        float survive=max(.05f,min(.95f,max(through.x,max(through.y,through.z))));
        if(randomValue(seed,bounce*4u+3u)>=survive)break;
        through=mul(through,1.0f/survive);
      }
      d=scatterDirection(d,rf(DATA_PASS 31u),randomValue(seed,bounce*4u+1u),randomValue(seed,bounce*4u+2u));
      o=add(p,mul(d,mix.cell*.00001f));
    }
  }
  result.color=mul(result.color,1.0f/float(inData[36]));return result;
}
FN RayResult mediumRay(DATA_ARG V o,V d,float nearT,float farT,V background,uint seed){
  RayResult result;result.color=v(0.0f,0.0f,0.0f);result.complete=1u;PREVIEW_PRECISE float transmittance=1.0f,opticalDepth=0.0f,compensation=0.0f,t=nearT;
  for(uint step=0u;step<inData[32];++step){
    if(t>=farT){break;}
    if(rf(DATA_PASS 46u)>0.0f && transmittance<rf(DATA_PASS 46u)){transmittance=0.0f;t=farT;break;}
    BoundSegment seg=raySegment(DATA_PASS o,d,t,farT);
    if(seg.start>=farT){t=farT;break;}
    if(seg.start>t){t=seg.start;continue;}
    float scale=rf(DATA_PASS 42u);if(scale<=0.0f)scale=.5f;
    MediumIntegral value=integrateMedium(DATA_PASS o,d,t,min(seg.cell*scale,seg.end-t),1u);
    if(value.complete==0u || value.dt<=0.0f || t+value.dt<=t){result.complete=0u;break;}
    if(rf(DATA_PASS 29u)>0.0f || rf(DATA_PASS 30u)>0.0f){
      PREVIEW_LOOP for(int refine=0;refine<12 && value.tau>.25f;++refine)
        value=integrateMedium(DATA_PASS o,d,t,value.dt*max(.001f,.25f/value.tau),1u);
    }
    if(value.complete==0u){result.complete=0u;break;}
    float absorbed=oneMinusExp(value.tau);
    if(absorbed>0.0f && value.tau>0.0f){
      float centroidOffset=value.tau<.01f?.5f-value.tau/12.0f:1.0f/value.tau-1.0f/(exp(min(80.0f,value.tau))-1.0f);
      V p=add(o,mul(d,t+value.dt*centroidOffset));
      V light=rf(DATA_PASS 29u)>0.0f?illumination(DATA_PASS p,v(0.0f,0.0f,0.0f),d,seg.cell,1u,seed+step*0x85ebca6bu,value.middle.field!=0u?inData[value.middle.field+11u]:0u):v(0.0f,0.0f,0.0f);
      light=add(mul(light,rf(DATA_PASS 29u)),v(rf(DATA_PASS 30u),rf(DATA_PASS 30u),rf(DATA_PASS 30u)));
      result.color=add(result.color,mul(had(mul(value.color,1.0f/value.tau),light),transmittance*absorbed));
      PREVIEW_PRECISE float delta=value.tau-compensation,next=opticalDepth+delta;compensation=(next-opticalDepth)-delta;opticalDepth=next;
      transmittance=exp(-min(80.0f,opticalDepth));
    }
    t+=value.dt;
  }
  if(t<farT && raySegment(DATA_PASS o,d,t,farT).start<farT)result.complete=0u;
  result.color=add(result.color,mul(background,transmittance));return result;
}
#ifndef PREVIEW_MODE
#define PREVIEW_MODE inData[0]
#define PREVIEW_MEDIUM inData[27]
#define PREVIEW_MULTIPLE (inData[35]>1u?1u:0u)
#endif
FN void work(uint id WORK_ARGS){
  if(PREVIEW_MODE==2u){
    if(id>=inData[1])return;
    uint lo=0u,hi=inData[2]-1u;
    while(lo<hi){uint mid=lo+(hi-lo)/2u,m=16u+mid*64u;if(id>=inData[m+48u]+inData[m+49u])lo=mid+1u;else hi=mid;}
    uint m=16u+lo*64u,j=id-inData[m+48u],weight=inData[m+51u]+j*3u;
    float w0=rf(DATA_PASS weight),w1=rf(DATA_PASS weight+1u),w2=rf(DATA_PASS weight+2u);
    float values[16];
    for(uint k=0u;k<16u;++k)values[k]=w0*rf(DATA_PASS m+k)+w1*rf(DATA_PASS m+16u+k)+w2*rf(DATA_PASS m+32u+k);
    V n=norm(v(values[4],values[5],values[6]));float alpha=values[3];
    values[3]=rf(DATA_PASS m+50u);values[4]=n.x;values[5]=n.y;values[6]=n.z;values[7]=alpha;
    for(uint k=0u;k<16u;++k)outData[id*16u+k]=values[k];
    return;
  }

  if(PREVIEW_MODE==0u){
    uint dim=inData[1],count=inData[2];
    if(id>=dim*dim*dim)return;
    float cell=rf(DATA_PASS 3);
    V p=v(float(id%dim),float((id/dim)%dim),float(id/(dim*dim)));
    float best=3.0f*cell;V color=v(.5f,.5f,.5f);
    for(uint i=0u;i<count;++i){
      uint off=16u+i*12u;
      V d=sub(p,rv(DATA_PASS off));
      float plane=dotv(d,rv(DATA_PASS off+4u));
      float radial=sqrt(max(0.0f,dotv(d,d)-plane*plane));
      float q0=radial-rf(DATA_PASS off+3u),q1=abs(plane)-.55f;
      float distance=(min(max(q0,q1),0.0f)+sqrt(max(q0,0.0f)*max(q0,0.0f)+max(q1,0.0f)*max(q1,0.0f)))*cell;
      if(distance<best){best=distance;color=rv(DATA_PASS off+8u);}
    }
    outData[id*4u]=best;outData[id*4u+1u]=color.x;outData[id*4u+2u]=color.y;outData[id*4u+3u]=color.z;
    return;
  }
  uint width=inData[1],height=inData[2],bricks=inData[3];
  if(id>=width*height)return;
  float sx=2.0f*(float(id%width)+.5f)/float(width)-1.0f;
  float sy=1.0f-2.0f*(float(id/width)+.5f)/float(height);
  V offset=add(mul(rv(DATA_PASS 12u),sx),mul(rv(DATA_PASS 16u),sy));
  V o=rv(DATA_PASS 4u),d=rv(DATA_PASS 8u);
  if(inData[7]!=0u)o=add(o,offset);else d=add(d,offset);
  d=norm(d);
  float facing=max(1e-6f,dotv(d,norm(rv(DATA_PASS 8u))));
  float nearClip=rf(DATA_PASS 23u)/facing,farClip=rf(DATA_PASS 24u)/facing;
  float nearest=farClip,uncertain=farClip;V color=rv(DATA_PASS 20u);uint hitField=0u;
  if(PREVIEW_MEDIUM!=0u){
    RayResult value=PREVIEW_MULTIPLE!=0u?multipleRay(DATA_PASS id,o,d,nearClip,farClip,color):mediumRay(DATA_PASS o,d,nearClip,farClip,color,id+inData[37]);
    outData[id*4u]=value.color.x;outData[id*4u+1u]=value.color.y;outData[id*4u+2u]=value.color.z;outData[id*4u+3u]=value.complete!=0u?1.0f:-1.0f;return;
  }
  for(uint b=0u;b<bricks;++b){
    uint m=64u+b*16u;
    float cell=rf(DATA_PASS m+3u);
    V lo=sub(rv(DATA_PASS m),v(.5f*cell,.5f*cell,.5f*cell));
    V hi=add(lo,v(float(inData[m+4u])*cell,float(inData[m+5u])*cell,float(inData[m+6u])*cell));
    float nearT=boxInterval(lo,hi,o,d,nearClip,nearest,0u),farT=boxInterval(lo,hi,o,d,nearClip,nearest,1u);
    if(nearT<0.0f)continue;
    float prevT=nearT,prev=grid(DATA_PASS m,add(o,mul(d,nearT)),0u);
    float hit=-1.0f,sampledThrough=nearT,scale=rf(DATA_PASS 42u);if(scale<=0.0f)scale=.5f;
    uint limit=inData[32]!=0u?inData[32]:4096u;
    for(uint j=0u;j<limit;++j){
      float t=min(farT,nearT+float(j+1u)*cell*scale);sampledThrough=t;
      float value=grid(DATA_PASS m,add(o,mul(d,t)),0u);
      if((prev<0.0f&&value>0.0f)||(prev>0.0f&&value<0.0f)){
        float a=prevT,c=t,fa=prev;
        for(int k=0;k<12;++k){
          float mid=(a+c)*.5f,fm=grid(DATA_PASS m,add(o,mul(d,mid)),0u);
          if((fa>0.0f)==(fm>0.0f)){a=mid;fa=fm;}else c=mid;
        }
        hit=(a+c)*.5f;break;
      }
      if(value!=0.0f){prev=value;prevT=t;}
      if(t>=farT)break;
    }
    if(hit<0.0f && sampledThrough<farT)uncertain=min(uncertain,sampledThrough);
    if(hit<0.0f||hit>=nearest)continue;
    nearest=hit;hitField=m;
  }
  if(hitField!=0u){
    uint m=hitField;float cell=rf(DATA_PASS m+3u);V p=add(o,mul(d,nearest));
    V dx=v(cell,0.0f,0.0f),dy=v(0.0f,cell,0.0f),dz=v(0.0f,0.0f,cell);
    V n=norm(v(grid(DATA_PASS m,add(p,dx),0u)-grid(DATA_PASS m,sub(p,dx),0u),
                 grid(DATA_PASS m,add(p,dy),0u)-grid(DATA_PASS m,sub(p,dy),0u),
                 grid(DATA_PASS m,add(p,dz),0u)-grid(DATA_PASS m,sub(p,dz),0u)));
    V light=illumination(DATA_PASS p,n,d,cell,0u,id+inData[37],0u);
    V albedo=v(grid(DATA_PASS m,p,1u),grid(DATA_PASS m,p,2u),grid(DATA_PASS m,p,3u));
    color=v(albedo.x*light.x,albedo.y*light.y,albedo.z*light.z);
  }
  outData[id*4u]=color.x;outData[id*4u+1u]=color.y;outData[id*4u+2u]=color.z;outData[id*4u+3u]=uncertain<nearest?-1.0f:1.0f;
}
