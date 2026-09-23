#!/usr/bin/env python3
"""Opt-in HDR regression, quality sweep and isolated RTX timing for fireflies.

Requires numpy. The executable is tests/pathtrace/remix-fireflies-test.
Outputs include exact launch settings, linear PFM captures and JSON measurements.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import numpy as np

LUMA = np.array([.2126, .7152, .0722])

def read(path):
    with path.open('rb') as f:
        assert f.readline().strip() == b'PF'
        w, h = map(int, f.readline().split())
        assert float(f.readline()) == -1
        a = np.frombuffer(f.read(), '<f4').reshape(h, w, 3)[::-1]
    assert np.isfinite(a).all() and (a >= 0).all(), f'Invalid HDR radiance: {path}'
    return a

def frames(path):
    return np.stack([read(f) for f in sorted(path.glob('frame-*.pfm'), key=lambda p: int(p.stem.split('-')[-1]))])

def roi(shape):
    # Interior of the back wall: four material rows, excluding the visible emitters.
    h, w = shape[-3:-1]
    mask = np.zeros((h, w), bool)
    mask[round(h*.175):round(h*.75), round(w*.19):round(w*.81)] = True
    return mask

def run(args, name, **settings):
    dest = args.output / name
    dest.mkdir(parents=True, exist_ok=True)
    env = {k:v for k,v in os.environ.items() if not k.startswith('REMIX_TEST_FIREFLY_')
           and k not in ('REMIX_PATH_LINEAR_CAPTURE', 'REMIX_RAY_PROFILE', 'REMIX_TRACE_STAGE_PROFILE', 'REMIX_STACK_PROFILE',
                         'REMIX_PATH_SAMPLE_SEED', 'REMIX_PATH_LINEAR_SIGNAL', 'REMIX_PATH_SAMPLING_PROFILE')}
    configured = {'REMIX_TEST_FIREFLY_'+k: str(v) for k,v in settings.items() if not k.startswith('REMIX_')}
    configured.update({k:str(v) for k,v in settings.items() if k.startswith('REMIX_')})
    if (dest/'success.json').exists():
        if json.loads((dest/'settings.json').read_text()) != configured:
            raise RuntimeError(f'{dest}: existing settings differ; use a new output directory')
        return dest
    if settings.get('BENCHMARK'):
        for key in ('REMIX_VULKAN_VALIDATION','VK_LAYER_VALIDATE_SYNC','VK_INSTANCE_LAYERS','VK_LOADER_DEBUG',
                    'REMIX_PBR_REUSE_DIAGNOSTICS','REMIX_LIGHTING_DIAGNOSTICS'):
            env.pop(key,None)
    env.update(configured)
    (dest / 'settings.json').write_text(json.dumps(configured, indent=2)+'\n')
    print(name, flush=True)
    with (dest/'run.log').open('w') as log:
        completed = subprocess.run([str(args.executable), str(dest)], env=env, stdout=log, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f'{name}: exit {completed.returncode}; see {dest}/run.log')
    (dest/'success.json').write_text(json.dumps({'exit':0})+'\n')
    return dest

def regression(args):
    results = []
    # Raw primary local clamping remains in outgoing luminance for all material rows.
    for i, (family, primary, split, samples, maps) in enumerate([
        ('torch','raster',0,1,1),('emissive','ray',0,4,1),('mixed','raster',1,1,1),
        ('mixed','ray',2,4,1),('mixed','raster',3,16,1),('torch','ray',0,4,0)]):
        common = dict(FAMILY=family, PRIMARY=primary, SPLIT=split, LOCAL_SAMPLES=samples,
                      MAPS=maps, BOUNCES=0, FRAMES=4)
        a = frames(run(args,f'cap-{i}',DIRECT=.1,**common))
        mask = roi(a.shape)
        maximum = float((a@LUMA)[:,mask].max())
        assert maximum <= .1001, (i, maximum)
        results.append({'case':i,'maximum_outgoing_luminance':maximum})
    for family in ('torch','emissive','mixed'):
        a=frames(run(args,f'small-{family}',FAMILY=family,LIGHT_COUNT=1,EMITTER_COUNT=1,
                     BOUNCES=0,DIRECT=.1,FRAMES=4))
        assert float((a@LUMA)[:,roi(a.shape)].max())<=.1001
    a=frames(run(args,'basic-triangles',FAMILY='emissive',CANDIDATES=1,BOUNCES=0,DIRECT=.1,FRAMES=4))
    assert float((a@LUMA)[:,roi(a.shape)].max())<=.1001
    # Independent indirect-only contribution, and bounded sum of two completed signals.
    a = frames(run(args,'cap-indirect',DIRECT=.1,INDIRECT=.05,BOUNCES=4,FRAMES=4))
    maximum = float((a@LUMA)[:,roi(a.shape)].max())
    assert maximum <= .1501, maximum
    # No-op large threshold and color preservation for a single completed sample.
    base = frames(run(args,'ratios-base',BOUNCES=0,FRAMES=4))
    high = frames(run(args,'ratios-high',BOUNCES=0,DIRECT=1000000,FRAMES=4))
    assert np.array_equal(base,high), 'Inactive high threshold changed samples'
    cap = frames(run(args,'ratios-cap',BOUNCES=0,DIRECT=.1,FRAMES=4))
    mask = roi(base.shape)
    expected = base * np.minimum(1, .1/np.maximum(base@LUMA, 1e-30))[...,None]
    assert np.allclose(cap[:,mask],expected[:,mask],rtol=2e-5,atol=1e-6), 'RGB ratios or outgoing threshold changed'
    # Both NRD lobes must receive the same clamp scale before packing. Half
    # precision and YCoCg conversion need a small quantization tolerance.
    lobes = {}
    for signal in ('diffuse','specular'):
        for mode, threshold in (('base',0),('cap',.1)):
            lobes[signal,mode] = frames(run(args,f'lobe-{signal}-{mode}',BOUNCES=0,DENOISER='reblur-sh',
                DIRECT=threshold,FRAMES=4,REMIX_PATH_LINEAR_SIGNAL=signal))
    diffuse = lobes['diffuse','base']; specular = lobes['specular','base']
    valid = roi(diffuse.shape)[None,...] & ((diffuse@LUMA)>.005) & ((specular@LUMA)>.005)
    diffuse_scale=(lobes['diffuse','cap']@LUMA)/np.maximum(diffuse@LUMA,1e-10)
    specular_scale=(lobes['specular','cap']@LUMA)/np.maximum(specular@LUMA,1e-10)
    assert np.allclose(diffuse_scale[valid],specular_scale[valid],atol=.004,rtol=.006), 'Lobe separation changed'
    sun = frames(run(args,'sun-emission-base',SUN=1,LOCAL=0,BOUNCES=0,FRAMES=4))
    sun_cap = frames(run(args,'sun-emission-cap',SUN=1,LOCAL=0,BOUNCES=0,DIRECT=.0001,INDIRECT=.0001,FRAMES=4))
    assert np.array_equal(sun,sun_cap), 'Primary sunlight or visible emission was clamped'
    distance = frames(run(args,'distance-base',FRAMES=4,REMIX_EMISSIVE_HIT_DISTANCE='distance'))
    distance_cap = frames(run(args,'distance-cap',DIRECT=.1,INDIRECT=.1,FRAMES=4,REMIX_EMISSIVE_HIT_DISTANCE='distance'))
    assert np.array_equal(distance,distance_cap), 'Clamping changed hit distances'
    # A finite sky tail contributes after an open front wall; the completed path stays bounded.
    sky = frames(run(args,'sky-cap',SKY=1,LOCAL=0,INDIRECT=.05,DIRECT=.1,FRAMES=4))
    assert float((sky@LUMA)[:,roi(sky.shape)].max())<=.0501, 'Sky tail escaped the indirect clamp'
    for mode,limit in [('base',0),('cap',.1)]:
        run(args,f'ray-budget-{mode}',DIRECT=limit,INDIRECT=limit,FRAMES=4,
            REMIX_RAY_PROFILE=str(args.output/f'rays-{mode}.csv'))
    ray_base=np.genfromtxt(args.output/'rays-base.csv',delimiter=',',skip_header=1)[:,1:]
    ray_cap=np.genfromtxt(args.output/'rays-cap.csv',delimiter=',',skip_header=1)[:,1:]
    assert np.array_equal(ray_base,ray_cap), 'Clamping changed ray or candidate budgets'
    # Matrix includes reuse requests, SHARC eligible legacy receivers, sheets and motion.
    cases = [dict(BOUNCES=0),dict(BOUNCES=4,PRIMARY='ray',SPLIT=2),
             dict(SHARC=1,PBR=0),dict(TRANSPARENT=1,WATER=1),dict(REUSE=1),
             dict(MOTION='camera'),dict(MOTION='light'),dict(MOTION='remove'),
             dict(UPSCALER='dlss',RR=0),dict(UPSCALER='dlss',RR=1),dict(LOCAL_DENOISER=0)]
    for i, settings in enumerate(cases):
        frames(run(args,f'matrix-{i}',DENOISER='reblur-sh',TAA=1,DIRECT=1,INDIRECT=1,FILTER=1,
                   FRAMES=8,**settings))
    # Exercise split indirect producer separately, including SHARC.
    for mode in ('0','1'):
        frames(run(args,f'indirect-{mode}',DENOISER='reblur-sh',TAA=1,SPLIT=3,DIRECT=1,INDIRECT=1,
                   REMIX_SPLIT_INDIRECT=mode,FRAMES=4))
    (args.output/'regression.json').write_text(json.dumps(results,indent=2)+'\n')

def quality(args):
    for family in ('torch','emissive','mixed'):
        for seed in (17011,93077):
            run(args,f'{family}-reference-{seed}',FAMILY=family,SAMPLES=16,LOCAL_SAMPLES=16,
                FRAMES=args.reference_frames,WARMUP=16,REMIX_PATH_SAMPLE_SEED=seed)
        for threshold in (0,.25,1,4,16):
            run(args,f'{family}-raw-{threshold}',FAMILY=family,DIRECT=threshold,INDIRECT=threshold,FRAMES=32)
        for direct,indirect in ((1,0),(0,1)):
            run(args,f'{family}-raw-direct{direct}-indirect{indirect}',FAMILY=family,DIRECT=direct,INDIRECT=indirect,FRAMES=32)
        for mode,direct,indirect,filtering in [('baseline',0,0,0),('clamp',1,1,0),('filter',0,0,1),('combined',1,1,1)]:
            run(args,f'{family}-{mode}',FAMILY=family,DENOISER='reblur-sh',TAA=1,SPLIT=3,
                DIRECT=direct,INDIRECT=indirect,FILTER=filtering,FRAMES=32,WARMUP=64)
    for pbr,maps in ((0,0),(1,0),(1,1)):
        run(args,f'maps-{pbr}-{maps}',PBR=pbr,MAPS=maps,FRAMES=32)
    analyze(args)

def refine(args):
    previous = json.loads((args.output/'quality.json').read_text())
    for family in ('torch','emissive','mixed'):
        if previous.get(family,{}).get('reference_converged'): continue
        for seed in (17011,93077):
            run(args,f'{family}-reference-refined-{args.reference_frames}-{seed}',FAMILY=family,SAMPLES=16,LOCAL_SAMPLES=16,
                AVERAGE=args.reference_frames,WARMUP=16,REMIX_PATH_SAMPLE_SEED=seed)
    analyze(args)

def analyze(args):
    report = {}
    for family in ('torch','emissive','mixed'):
        dirs = [args.output/f'{family}-reference-{s}' for s in (17011,93077)]
        refined = [args.output/f'{family}-reference-refined-{s}' for s in (17011,93077)]
        if all((d/'success.json').exists() for d in refined): dirs = refined
        for candidate in args.output.glob(f'{family}-reference-refined-*-17011'):
            count=int(candidate.name.split('-')[-2])
            pair=[args.output/f'{family}-reference-refined-{count}-{s}' for s in (17011,93077)]
            if all((d/'success.json').exists() for d in pair):
                old_count=int(json.loads((dirs[0]/'settings.json').read_text()).get('REMIX_TEST_FIREFLY_AVERAGE',256))
                if count>old_count: dirs=pair
        if not all((d/'success.json').exists() for d in dirs):
            continue
        means = [frames(d).mean(0) for d in dirs]
        ref = (means[0]+means[1])*.5
        mask = roi(ref.shape)
        yref=ref@LUMA
        highlights = mask & (yref >= np.quantile(yref[mask],.95))
        convergence = float(abs((means[0]@LUMA)[mask].sum()-(means[1]@LUMA)[mask].sum())/yref[mask].sum())
        convergence_highlight = float(abs((means[0]@LUMA)[highlights].sum()-(means[1]@LUMA)[highlights].sum())/yref[highlights].sum())
        regional_convergence=[]
        for rows in np.array_split(np.flatnonzero(mask.any(1)),4):
            region=mask & np.isin(np.indices(mask.shape)[0],rows)
            regional_convergence.append(float(abs((means[0]@LUMA)[region].sum()-(means[1]@LUMA)[region].sum())/yref[region].sum()))
        metrics={}
        for d in args.output.glob(f'{family}-*'):
            if 'reference' in d.name or not (d/'success.json').exists(): continue
            a=frames(d)@LUMA
            outliers=(a > np.maximum(8*yref,yref+2)) & mask
            mean=a.mean(0)
            regional=[]
            for rows in np.array_split(np.arange(mask.shape[0])[mask.any(axis=1)],4):
                region=mask.copy();region[:rows[0]]=False;region[rows[-1]+1:]=False
                regional.append(float(abs(mean[region].sum()-yref[region].sum())/yref[region].sum()))
            metrics[d.name]={'extreme_positive_outliers':float(outliers[:,mask].mean()),
                'temporal_rms':float(np.sqrt(np.mean(np.diff(a[:,mask],axis=0)**2))),
                'mean_energy_error':float(abs(mean[mask].sum()-yref[mask].sum())/yref[mask].sum()),
                'mean_energy_signed_error':float(mean[mask].sum()/yref[mask].sum()-1),
                'regional_mean_energy_errors':regional,
                'highlight_energy_loss':float(max(0,1-mean[highlights].sum()/yref[highlights].sum()))}
        base=metrics[f'{family}-raw-0']['extreme_positive_outliers']
        for name,m in metrics.items():
            m['outlier_reduction_vs_raw']=1-m['extreme_positive_outliers']/base if base else None
            m['meets_initial_targets']=(m['outlier_reduction_vs_raw'] is not None and m['outlier_reduction_vs_raw']>=.75
                and max(m['regional_mean_energy_errors'])<=.02 and m['highlight_energy_loss']<=.05)
        report[family]={'reference_directories':[str(d.name) for d in dirs],
                        'reference_regional_mean_disagreement':convergence,
                        'reference_highlight_disagreement':convergence_highlight,
                        'reference_region_disagreements':regional_convergence,
                        'reference_converged':max(regional_convergence)<=.01 and convergence_highlight<=.025,'modes':metrics}
    (args.output/'quality.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

def timing(args):
    report={}
    modes={'baseline':(0,0,0),'clamp':(1,1,0),'filter':(0,0,1),'combined':(1,1,1)}
    for candidate in ('clamp','filter','combined'):
        pairs=[]
        for pair in range(3):
            order=('baseline',candidate) if pair%2==0 else (candidate,'baseline')
            values={}
            for mode in order:
                direct,indirect,filtering=modes[mode]
                d=run(args,f'timing-{candidate}-{pair}-{mode}',BENCHMARK=1,DENOISER='reblur-sh',TAA=1,SPLIT=3,
                      DIRECT=direct,INDIRECT=indirect,FILTER=filtering,REUSE=int(args.reuse))
                data=np.genfromtxt(d/'timing.csv',delimiter=',',names=True)
                total=sum(data[key] for key in data.dtype.names)
                values[mode]={'median_gpu_ms':float(np.median(total)), 'frames':int(total.size)}
            delta=values[candidate]['median_gpu_ms']/values['baseline']['median_gpu_ms']-1
            pairs.append({'runs':values,'relative_change':delta})
        report[candidate]={'pairs':pairs,'repeatable_regression_over_2_percent':all(p['relative_change']>.02 for p in pairs)}
        (args.output/'timing.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

def bypass(args):
    # A very large limit minimizes clipping while measuring the full reuse bypass.
    report={}
    for kind,common in [('pbr',dict(REUSE=1)),('water',dict(TRANSPARENT=2))]:
        pairs=[]
        for pair in range(3):
            values={}
            order=('baseline','bypass') if pair%2==0 else ('bypass','baseline')
            for mode in order:
                limit=1000000 if mode=='bypass' else 0
                d=run(args,f'{kind}-{pair}-{mode}',BENCHMARK=1,DENOISER='reblur-sh',TAA=1,SPLIT=3,
                      DIRECT=limit if kind=='pbr' else 0,INDIRECT=limit if kind=='water' else 0,**common)
                data=np.genfromtxt(d/'timing.csv',delimiter=',',names=True)
                values[mode]=float(np.median(sum(data[k] for k in data.dtype.names)))
            pairs.append({'gpu_ms':values,'relative_change':values['bypass']/values['baseline']-1})
        changes=[p['relative_change'] for p in pairs]
        consistent=all(x>0 for x in changes) or all(x<0 for x in changes)
        report[kind]={'pairs':pairs,'repeatable_regression_over_2_percent':all(x>.02 for x in changes),
                      'direction_consistent':consistent,'range_percent':[100*min(changes),100*max(changes)],
                      'inconclusive':not consistent and max(changes)-min(changes)>.04}
        (args.output/'bypass.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=['regression','quality','refine','analyze','timing','bypass'])
    p.add_argument('--executable',type=lambda p:Path(p).absolute(),required=True)
    p.add_argument('--output',type=lambda p:Path(p).absolute(),required=True)
    p.add_argument('--reference-frames',type=int,default=256)
    p.add_argument('--reuse',action='store_true',help='Timing with experimental PBR reuse requested')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    globals()[args.mode](args)
