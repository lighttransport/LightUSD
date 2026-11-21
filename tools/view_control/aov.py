"""Raw PFM depth/normal comparison with explicit coordinate and validity rules."""
from array import array
import math
from pathlib import Path
import sys
from PIL import Image,ImageChops,ImageFilter


def read_pfm(path):
    with Path(path).open('rb') as stream:
        if stream.readline().strip()!=b'PF': raise ValueError('expected RGB PFM')
        width,height=map(int,stream.readline().split());scale=float(stream.readline())
        if not 1<=width<=4096 or not 1<=height<=4096 or abs(scale)!=1: raise ValueError('invalid PFM header')
        raw=stream.read(width*height*12+1)
    if len(raw)!=width*height*12: raise ValueError('invalid PFM data length')
    data=array('f');data.frombytes(raw)
    if (scale<0)!=(sys.byteorder=='little'): data.byteswap()
    # Canonical top row first, matching PNG pixels and camera ray convention.
    top=array('f')
    for row in range(height-1,-1,-1):top.extend(data[row*width*3:(row+1)*width*3])
    if any(not math.isfinite(v) for v in top): raise ValueError('non-finite AOV sample')
    return (width,height),top


def compare_data(reference,candidate,camera,aov,out,gates,depth_reference=None,depth_candidate=None):
    size,r=read_pfm(reference);other,c=read_pfm(candidate)
    if size!=other: raise ValueError('AOV dimensions differ')
    w,h=size;near,far=camera['near'],camera['far'];tan=math.tan(math.radians(camera['fov_y'])/2)
    rm=[];cm=[];rv=[];cv=[];normal=aov=='worldNormal';matrix=camera['transform']
    for i in range(w*h):
        ref=r[3*i:3*i+3];cand=c[3*i:3*i+3]
        if normal:
            rn=math.sqrt(sum(v*v for v in ref));cn=math.sqrt(sum(v*v for v in cand))
            rm.append(rn>.1);cm.append(cn>.1)
            # Storm Neye -> world using the frozen camera rotation; native is geometric world normal.
            ref=[sum(ref[k]*matrix[4*k+j] for k in range(3))/max(rn,1e-30) for j in range(3)]
            cand=[v/max(cn,1e-30) for v in cand]
        else:
            d=ref[0];rm.append(0<=d<1-1e-7);cm.append(cand[0]>0)
            z=near+(far-near)*d if camera['projection']=='orthographic' else near*far/(far-d*(far-near))
            x,y=i%w,i//w
            # OpenGL depth is axial; native depth is distance along a normalized primary ray.
            px=(2*(x+.5)/w-1+camera['shift'][0])*tan*camera['aspect']
            py=(1-2*(y+.5)/h+camera['shift'][1])*tan
            ref=z if camera['projection']=='orthographic' else z*math.sqrt(1+px*px+py*py)
            cand=cand[0]
        rv.append(ref);cv.append(cand)
    am=Image.frombytes('L',size,bytes(255 if v else 0 for v in rm));bm=Image.frombytes('L',size,bytes(255 if v else 0 for v in cm))
    intersection=ImageChops.multiply(am,bm);union=ImageChops.lighter(am,bm)
    union_count=union.histogram()[255];common=intersection.histogram()[255]
    interior=intersection.filter(ImageFilter.MinFilter(3)).tobytes();errors=[];heat=bytearray();preview_r=bytearray();preview_c=bytearray()
    for i,valid in enumerate(interior):
        if normal:
            error=math.degrees(math.acos(max(-1,min(1,sum(a*b for a,b in zip(rv[i],cv[i]))))))
            for values,target in ((rv[i],preview_r),(cv[i],preview_c)):
                target.extend(round(255*max(0,min(1,v*.5+.5))) for v in values)
            display_error=min(1,error/30)
        else:
            error=abs(rv[i]-cv[i])/max(abs(rv[i]),1e-8)
            for value,target in ((rv[i],preview_r),(cv[i],preview_c)):
                gray=round(255*max(0,min(1,(value-near)/(far-near))));target.extend([gray]*3)
            display_error=min(1,error/.05)
        if valid:errors.append(error)
        heat.extend((round(255*display_error),0,0) if valid else (0,0,0))
    errors.sort();p95=errors[min(len(errors)-1,int(.95*len(errors)))] if errors else None
    iou=common/union_count if union_count else None
    threshold=gates['normal_p95_degrees_max' if normal else 'depth_relative_p95_max']
    validity={}
    normal_iou=iou
    if normal and (depth_reference is not None or depth_candidate is not None):
        if depth_reference is None or depth_candidate is None:raise ValueError('both depth coverage inputs are required')
        rs,rd=read_pfm(depth_reference);cs,cd=read_pfm(depth_candidate)
        if rs!=size or cs!=size:raise ValueError('depth/normal dimensions differ')
        rg=[0<=rd[i*3]<1-1e-7 for i in range(w*h)];cg=[cd[i*3]>0 for i in range(w*h)]
        reference_interior=Image.frombytes('L',size,bytes(255 if v else 0 for v in rg)).filter(ImageFilter.MinFilter(3)).tobytes()
        candidate_interior=Image.frombytes('L',size,bytes(255 if v else 0 for v in cg)).filter(ImageFilter.MinFilter(3)).tobytes()
        union_geometry=sum(a or b for a,b in zip(rg,cg))
        iou=sum(a and b for a,b in zip(rg,cg))/union_geometry if union_geometry else None
        validity=dict(normal_coverage_iou=normal_iou,
                      reference_invalid_normal_pixels=sum(bool(v) and not rm[i]for i,v in enumerate(reference_interior)),
                      candidate_invalid_normal_pixels=sum(bool(v) and not cm[i]for i,v in enumerate(candidate_interior)),
                      validity_contract='Interior depth hits require a finite, nonzero normal; each engine checked independently')
    comparison_passed=bool(errors) and iou is not None and iou>=gates['coverage_iou_min'] and p95<=threshold
    passed=comparison_passed and not validity.get('reference_invalid_normal_pixels',0) and not validity.get('candidate_invalid_normal_pixels',0)
    out=Path(out);out.mkdir(exist_ok=True)
    for name,pixels in [('reference',preview_r),('candidate',preview_c),('error',heat)]:Image.frombytes('RGB',size,bytes(pixels)).save(out/(name+'.png'))
    return {'passed':passed,'comparison_passed':comparison_passed,**validity,'coverage_iou':iou,'common_pixels':common,'interior_pixels':len(errors),
            'reference_coverage_pixels':sum(rm),'candidate_coverage_pixels':sum(cm),
            'reference_empty':not any(rm),
            'mean_error':sum(errors)/len(errors) if errors else None,'p95_error':p95,
            'units':'degrees' if normal else 'relative ray distance', 'gates':gates,
            'semantics':'SDK eye-space pre-material interpolated normal vs native world-space authored interpolated normal; material normal maps are excluded. Backface conventions require calibration.' if normal else 'OpenGL window depth converted to primary-ray distance in stage units; background excluded. No alignment.',
            'images':{name:str(out/(name+'.png')) for name in ('reference','candidate','error')}}
