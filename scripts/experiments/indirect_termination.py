#!/usr/bin/env python3
"""GPU regression and HDR comparisons for adaptive indirect termination.

Use separate output directories for regression, quality and denoiser captures.
Timing is performed by BetaRT's snapshot profiler, without this instrumentation.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import numpy as np
import fireflies as hdr


def run(args, name, **settings):
    out = hdr.run(args, name, **settings)
    log = (out / 'run.log').read_text()
    assert not any(s in log for s in ('Validation Error', 'SYNC-HAZARD', 'VUID-',
                                     'Adaptive termination unavailable', 'Separate indirect pass unavailable',
                                     'Separate indirect consumer unavailable')), out
    return out


def regression(args):
    common = dict(FRAMES=3, WARMUP=8, LOCAL_SAMPLES=1, REMIX_PATH_SAMPLE_SEED=17011)
    results = []
    for bounces, samples in [(0, 1), (1, 4), (2, 1), (2, 4), (4, 16)]:
        settings = dict(common, BOUNCES=bounces, SAMPLES=samples)
        label = f'b{bounces}-s{samples}'
        base = hdr.frames(run(args, label + '-fixed', **settings))
        inactive = hdr.frames(run(args, label + '-inactive', ROULETTE=1,
                                  ROULETTE_MIN=max(1, bounces), **settings))
        assert np.array_equal(base, inactive), f'Inactive adaptation changed samples: {label}'
        results.append(dict(case=label, inactive_equal=True))
        if args.baseline_library:
            previous = os.environ.get('LD_LIBRARY_PATH')
            os.environ['LD_LIBRARY_PATH'] = str(args.baseline_library.resolve()) + ':' + (previous or '')
            try:
                old = hdr.frames(run(args, label + '-previous', BASELINE=1, **settings))
            finally:
                if previous is None:
                    os.environ.pop('LD_LIBRARY_PATH', None)
                else:
                    os.environ['LD_LIBRARY_PATH'] = previous
            assert np.array_equal(base, old), f'Disabled mode differs from committed baseline: {label}'
            results[-1]['committed_baseline_equal'] = True
    distances = [hdr.frames(run(args, f'first-distance-{mode}', BOUNCES=4, ROULETTE=mode,
                               LOCAL=0, SKY=1, DENOISER='reblur-sh',
                               REMIX_PATH_LINEAR_SIGNAL='hit-distance', **common)) for mode in (0, 1)]
    assert np.array_equal(*distances), 'Adaptive termination changed the first-hit distance'
    results.append(dict(case='first-distance', equal=True))
    lobes = {}
    for mode in (0, 1):
        for signal in ('diffuse', 'specular'):
            lobes[mode, signal] = hdr.frames(run(args, f'lobe-{mode}-{signal}', BOUNCES=4,
                ROULETTE=mode, LOCAL=0, SKY=1, DENOISER='reblur-sh',
                REMIX_PATH_LINEAR_SIGNAL=signal, **common))
    # NRD's half-precision YCoCg can lose tiny saturated RGB channels to
    # cancellation. Compare lobe luminance to isolate compensation from that
    # encoding error; the unclamped HDR reference checks retain RGB metrics.
    totals = [(lobes[m, 'diffuse'] + lobes[m, 'specular']) @ hdr.LUMA for m in (0, 1)]
    ratios = [(lobes[m, 'specular'] @ hdr.LUMA) / np.maximum(totals[m], 1e-10) for m in (0, 1)]
    valid = (hdr.roi(lobes[0, 'diffuse'].shape)[None, ...] & (totals[0] > .005) & (totals[1] > .005) &
             (ratios[0] > .05) & (ratios[0] < .95))
    assert valid.sum() > 100, 'Insufficient mixed-lobe coverage'
    assert np.allclose(ratios[0][valid], ratios[1][valid], atol=.004, rtol=.006), 'Lobes received different roulette compensation'
    results.append(dict(case='lobe-separation', compared_samples=int(valid.sum())))
    for bounces in (0, 1, 2, 4):
        for mode in (0, 1):
            name = f'counts-b{bounces}-{mode}'
            path = args.output.resolve() / (name + '.csv')
            run(args, name, **dict(common, BOUNCES=bounces, ROULETTE=mode,
                                  SAMPLES=4, REMIX_RAY_PROFILE=str(path)))
            rows = list(csv.DictReader(path.open()))
            totals = {k: sum(int(row[k]) for row in rows) for k in rows[0] if k != 'time_ms'}
            for domain in ('opaque', 'transparency'):
                prefix = domain + '_path_'
                get = lambda key: totals[prefix + key]
                assert get('starts') == sum(get(k) for k in ('miss', 'zero', 'cache', 'limit', 'sheet_limit', 'roulette_terminated'))
                assert get('roulette_tests') == get('roulette_survivals') + get('roulette_terminated')
                assert all(get('depth' + str(d)) == 0 for d in range(bounces + 1, 5))
                if not mode or bounces <= 1:
                    assert get('roulette_tests') == 0
            if mode and bounces > 1:
                assert totals['opaque_path_roulette_terminated'] > 0
            results.append(dict(case=name, counters=totals))
    depths = {}
    for minimum in (4, 2, 3):
        name = f'minimum-{minimum}'
        path = args.output / (name + '.csv')
        run(args, name, **dict(common, BOUNCES=4, ROULETTE=1, ROULETTE_MIN=minimum,
                              SAMPLES=1, REMIX_RAY_PROFILE=str(path)))
        rows = list(csv.DictReader(path.open()))
        depths[minimum] = [sum(int(row[f'opaque_path_depth{d}']) for row in rows) for d in range(1, 5)]
        if minimum < 4:
            assert depths[minimum][:minimum] == depths[4][:minimum], 'Minimum depth changed the path prefix'
            assert depths[minimum][minimum] < depths[4][minimum], 'Minimum did not allow termination'
    results.append(dict(case='minimum-bounces', depths=depths))
    # Coverage is shared through continuation(), including confidence replay.
    cases = [dict(PRIMARY='ray'), dict(TRANSPARENT=1), dict(PBR=0, TRANSPARENT=1, WATER=1),
             dict(SHARC=1), dict(SHARC=1, REMIX_SPLIT_INDIRECT=1), dict(REMIX_SPLIT_INDIRECT=0),
             dict(MOTION='camera'), dict(MOTION='light'), dict(MOTION='remove'),
             dict(UPSCALER='fsr31'), dict(UPSCALER='dlss', RR=0), dict(UPSCALER='dlss', RR=1),
             dict(DIRECT=.1, INDIRECT=.05), dict(REUSE=1), dict(ACCUMULATE=1), dict(MAPS=0)]
    for i, extra in enumerate(cases):
        settings = dict(BOUNCES=4, ROULETTE=1, DENOISER='reblur-sh', TAA=1, FILTER=1,
                        SPLIT=3, FRAMES=8, WARMUP=64, **extra)
        out = run(args, f'integration-{i}', **settings)
        hdr.frames(out)
        text = (out / 'state.txt').read_text()
        if extra.get('UPSCALER') == 'dlss':
            assert 'reconstruction=' + ('3' if extra['RR'] else '2') in text, text
            assert 'reconstruction_failures=0' in text, text
        results.append(dict(case=f'integration-{i}', state=text))
    (args.output / 'regression.json').write_text(json.dumps(results, indent=2) + '\n')


def masks(shape):
    h, w = shape[-3:-1]
    interior = hdr.roi(shape)
    regions = {}
    # The back wall at z=4 is three units from the camera, spanning +/-2.
    # Keep each material patch separate, trim its boundary, and retain the ROI
    # exclusion of the foreground emitter quads at the bottom of the image.
    ys, xs = np.linspace(h / 6, h * 5 / 6, 5), np.linspace(w / 6, w * 5 / 6, 6)
    for y, material in enumerate(('metal', 'saturated', 'dark', 'gray')):
        for x, roughness in enumerate((.03, .08, .2, .5, .85)):
            m = np.zeros((h, w), bool)
            y0, y1 = int(np.ceil(ys[y])) + 1, int(np.floor(ys[y+1])) - 1
            x0, x1 = int(np.ceil(xs[x])) + 1, int(np.floor(xs[x+1])) - 1
            m[y0:y1, x0:x1] = True
            regions[f'{material}-roughness{roughness}'] = m & interior
    return regions


def compare(a, b):
    regions = masks(a.shape)
    region_errors = {name: ((b[m].mean(axis=0) - a[m].mean(axis=0)) /
                            np.maximum(a[m].mean(axis=0), 1e-6)).tolist() for name, m in regions.items()}
    interior = hdr.roi(a.shape)
    luminance = a @ hdr.LUMA
    highlights = interior & (luminance >= np.quantile(luminance[interior], .95))
    return dict(regional_rgb_relative=region_errors,
                worst_regional_error=max(abs(v) for values in region_errors.values() for v in values),
                highlight_energy_error=float(((b @ hdr.LUMA)[highlights].mean() /
                                              max(luminance[highlights].mean(), 1e-8)) - 1),
                mean_energy_error=float((b @ hdr.LUMA)[interior].mean() / max(luminance[interior].mean(), 1e-8) - 1),
                normalized_rms=float(np.sqrt(np.sum((a[interior] - b[interior])**2) /
                                             max(np.sum(a[interior]**2), 1e-10))))


def quality(args):
    results = {}
    cases = [('torch', dict(FAMILY='torch')), ('emissive', dict(FAMILY='emissive')),
             ('mixed', dict(FAMILY='mixed')), ('transparent', dict(TRANSPARENT=1)),
             ('sky', dict(LOCAL=0, SKY=1)), ('legacy', dict(PBR=0)),
             ('sun', dict(LOCAL=0, SUN=1, FAMILY='torch'))]
    for name, extra in cases:
        images = {}
        for mode, seed, frames in [('reference-a', 17011, args.reference_frames),
                                   ('reference-b', 93077, args.reference_frames),
                                   ('adaptive-a', 57331, args.reference_frames),
                                   ('adaptive-b', 82787, args.reference_frames)]:
            out = run(args, f'{name}-{mode}-{frames}', BOUNCES=4, SAMPLES=16, LOCAL_SAMPLES=16,
                      ROULETTE=int(mode.startswith('adaptive')), AVERAGE=frames, WARMUP=8,
                      REMIX_PATH_SAMPLE_SEED=seed, **extra)
            images[mode] = hdr.frames(out)[0]
        # Independently seeded references must also be stable as their sample
        # count increases; seed agreement alone can miss slow convergence.
        coarse = []
        for seed in (17011, 93077):
            out = run(args, f'{name}-coarse-{seed}-{args.reference_frames // 2}',
                      BOUNCES=4, SAMPLES=16, LOCAL_SAMPLES=16, ROULETTE=0,
                      AVERAGE=args.reference_frames // 2, WARMUP=8,
                      REMIX_PATH_SAMPLE_SEED=seed, **extra)
            coarse.append(hdr.frames(out)[0])
        convergence = compare(images['reference-a'], images['reference-b'])
        refinement = compare((coarse[0] + coarse[1]) / 2,
                             (images['reference-a'] + images['reference-b']) / 2)
        comparison = compare((images['reference-a'] + images['reference-b']) / 2,
                             (images['adaptive-a'] + images['adaptive-b']) / 2)
        converged = all(m['worst_regional_error'] <= .02 and abs(m['highlight_energy_error']) <= .05
                        for m in (convergence, refinement))
        passed = converged and comparison['worst_regional_error'] <= .02 and abs(comparison['highlight_energy_error']) <= .05
        results[name] = dict(converged=converged, passed=passed, convergence=convergence,
                             refinement=refinement, comparison=comparison)
        (args.output / 'quality.json').write_text(json.dumps(results, indent=2) + '\n')
        print(name, 'converged', converged, 'passed', passed, flush=True)


def denoising(args):
    results = {}
    for name, extra in [('static', {}), ('camera', dict(MOTION='camera')),
                        ('light', dict(MOTION='light')), ('remove', dict(MOTION='remove'))]:
        images = {}
        for mode in (0, 1):
            out = run(args, f'{name}-{mode}', DENOISER='reblur-sh', TAA=1, FILTER=1, SPLIT=3,
                      BOUNCES=2, ROULETTE=mode, WARMUP=128, FRAMES=64, **extra)
            images[mode] = hdr.frames(out)
        a, b = images[0], images[1]
        mask = hdr.roi(a.shape)
        la, lb = a @ hdr.LUMA, b @ hdr.LUMA
        scale = max(float(la[:, mask].mean()), 1e-8)
        reference = la.mean(axis=0)
        extreme = np.maximum(reference * 8, scale * 10)
        results[name] = dict(comparison=compare(a.mean(axis=0), b.mean(axis=0)),
            temporal_rms_fixed=float(np.sqrt(np.mean(np.diff(la[:, mask], axis=0)**2)) / scale),
            temporal_rms_adaptive=float(np.sqrt(np.mean(np.diff(lb[:, mask], axis=0)**2)) / scale),
            extreme_fixed=int((la[:, mask] > extreme[mask]).sum()),
            extreme_adaptive=int((lb[:, mask] > extreme[mask]).sum()))
        (args.output / 'denoising.json').write_text(json.dumps(results, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('regression', 'quality', 'denoising'))
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-library', type=Path)
    parser.add_argument('--reference-frames', type=int, default=512)
    args = parser.parse_args()
    args.executable = args.executable.resolve()
    args.output = args.output.resolve()
    if args.reference_frames < 2:
        parser.error('Reference frame count must be at least two')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.baseline_library:
        library = args.baseline_library.resolve() / 'libremix.so.0'
        if not library.exists():
            parser.error('Baseline directory must contain libremix.so.0 (or its SONAME symlink)')
        env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
        linked = subprocess.check_output(['ldd', str(args.executable)], env=env, text=True)
        if str(library) not in linked:
            parser.error('Fixture does not resolve libremix.so.0 from the baseline directory')
        (args.output / 'baseline-loader.txt').write_text(linked)
    (args.output / 'fixture.json').write_text(json.dumps(dict(arguments=vars(args),
        executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest()), indent=2, default=str) + '\n')
    globals()[args.mode](args)


if __name__ == '__main__':
    main()
