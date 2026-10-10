"""Build the supported FSR 3.1 Vulkan permutations and reflect their descriptors."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / 'third_party/fsr31'
TARGET = ROOT / 'src/native/upscale/shaders/fsr31.h'
# Pass files in the order of each effect's pass enumeration (FfxFsr3UpscalerPass,
# FfxFrameInterpolationPass, FfxOpticalflowPass): the blob of a pass is found
# by that index (upscale/shader_blobs.cpp).
UPSCALER = ['prepare_inputs', 'luma_pyramid', 'shading_change_pyramid', 'shading_change',
            'prepare_reactivity', 'luma_instability', 'accumulate', 'accumulate',
            'rcas', 'debug_view', 'autogen_reactive']
INTERPOLATION = ['reconstruct_and_dilate_pass', 'setup_pass', 'reconstruct_previous_depth_pass',
                 'game_motion_vector_field_pass', 'optical_flow_vector_field_pass', 'disocclusion_mask_pass',
                 'pass', 'compute_inpainting_pyramid_pass', 'inpainting_pass',
                 'compute_game_vector_field_inpainting_pyramid_pass', 'debug_view_pass']
OPTICAL_FLOW = ['prepare_luma_pass', 'compute_luminance_pyramid_pass', 'generate_scd_histogram_pass',
                'compute_scd_divergence_pass', 'compute_optical_flow_advanced_pass_v5',
                'filter_optical_flow_pass_v5', 'scale_optical_flow_advanced_pass_v5']
COMMON = {'FFX_GLSL': 1, 'FFX_GPU': 1, 'FFX_HALF': 0}


def permutations():
    """(effect directory, pass file, defines) of every embedded blob, in blob
    order, and the first blob of each group as (constant name, index)."""
    result, groups = [], []
    # Upscaler: HDR input, inverted depth, unjittered render-resolution motion;
    # one set per reprojection kernel (reference Lanczos, then the LUT).
    groups.append(('upscalerBlobs', len(result)))
    for lut in range(2):
        for index, stage in enumerate(UPSCALER):
            result.append(('fsr3upscaler', f'ffx_fsr3upscaler_{stage}_pass', {
                **COMMON,
                'FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF': 0,
                'FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF': 0,
                'FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF': 1,
                'FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF': 0,
                'FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE': 2,
                'FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE': lut,
                'FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT': 1,
                'FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS': 1,
                'FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS': 0,
                'FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH': 1,
                'FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING': int(index == 7)}))
    # Frame interpolation: unjittered render-resolution motion; one set per
    # depth direction, because the depth it is given is the active upscaler's
    # (device depth with DLSS, reversed with FSR).
    groups.append(('frameInterpolationBlobs', len(result)))
    for inverted in range(2):
        for stage in INTERPOLATION:
            result.append(('frameinterpolation', f'ffx_frameinterpolation_{stage}', {
                **COMMON,
                'FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF': 0,
                'FFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF': 0,
                'FFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF': 1,
                'FFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF': 0,
                'FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE': 2,
                'FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS': 1,
                'FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS': 0,
                'FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH': inverted}))
    groups.append(('opticalFlowBlobs', len(result)))
    for stage in OPTICAL_FLOW:
        result.append(('opticalflow', f'ffx_opticalflow_{stage}',
                       {**COMMON, 'FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT': 0}))
    return result, groups


def reflect(words):
    """Descriptors of a module in FfxShaderBlob's group order: constant
    buffers, sampled textures, storage textures, read-only storage buffers,
    written storage buffers, samplers, acceleration structures; each a list of
    (name, binding, count, set)."""
    names, decorations, types, variables, constants = {}, {}, {}, {}, {}
    members, readonly = {}, {}
    offset = 5
    while offset < len(words):
        count, op = words[offset] >> 16, words[offset] & 65535
        args = words[offset+1:offset+count]
        if op == 5:
            names[args[0]] = struct.pack('<' + 'I' * (len(args)-1), *args[1:]).split(b'\0')[0].decode()
        elif op == 71:
            decorations.setdefault(args[0], {})[args[1]] = args[2:]
        elif op == 72 and args[2] == 24:  # OpMemberDecorate NonWritable
            readonly.setdefault(args[0], set()).add(args[1])
        elif op in (25, 26, 27, 28, 29, 30, 32):
            types[args[0]] = (op, args[1:])
            if op == 30:
                members[args[0]] = len(args) - 1
        elif op == 59:
            variables[args[1]] = (args[0], args[2])
        elif op == 43:
            constants[args[1]] = args[2]
        offset += count
    groups = [[] for _ in range(7)]
    for ident, (type_id, storage) in variables.items():
        deco = decorations.get(ident, {})
        if 33 not in deco:
            continue
        pointee = types[type_id][1][1]
        op, args = types[pointee]
        count = 1
        if op == 28:
            count = constants[args[1]]
            pointee = args[0]
            op, args = types[pointee]
        # A buffer block: the StorageBuffer class, or before SPIR-V 1.3 the
        # Uniform class with a BufferBlock type. `readonly` reaches SPIR-V as
        # NonWritable on every member; the backend binds such a block from its
        # read-only buffer table and any other from the written one.
        buffer = op == 30 and (storage == 12 or (storage == 2 and 3 in decorations.get(pointee, {})))
        if buffer:
            group = 3 if len(readonly.get(pointee, ())) == members[pointee] else 4
        elif storage == 2 and op == 30:
            group = 0
        elif op == 25:
            group = 1 if args[5] == 1 else 2
        elif op == 26:
            group = 5
        else:
            raise RuntimeError(f'Unsupported FSR descriptor: {names.get(ident)} {op} {storage}')
        groups[group].append((names[ident], deco[33][0], count, deco.get(34, [0])[0]))
    return [sorted(group, key=lambda item: item[1]) for group in groups]


def process(check=False, validate=False):
    if validate:
        with tempfile.TemporaryDirectory() as directory:
            for match in re.finditer(r'uint32_t shader\d+\[\] = \{(.*?)\};', TARGET.read_text(), re.S):
                words = [int(v, 16) for v in re.findall(r'0x[0-9a-f]+', match[1])]
                binary = Path(directory) / 'shader.spv'
                binary.write_bytes(struct.pack('<' + 'I'*len(words), *words))
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(binary)], check=True)
        return
    output = '// Generated by scripts/shaders/generate.py fsr31; do not edit.\n#pragma once\n'
    output += '#include <FidelityFX/host/ffx_types.h>\nnamespace remix::upscale {\n'
    blobs = []
    stages, starts = permutations()
    with tempfile.TemporaryDirectory() as directory:
        for number, (effect, stage, defines) in enumerate(stages):
            prefix = f'shader{number}'
            binary = Path(directory) / 'shader.spv'
            subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-Os',
                *[f'-D{k}={v}' for k,v in defines.items()],
                '-I'+str(VENDOR/'include/FidelityFX/gpu'),
                '-I'+str(VENDOR/'include/FidelityFX/gpu'/effect),
                str(VENDOR/f'src/backends/vk/shaders/{effect}/{stage}.glsl'),
                '-o', str(binary)], check=True)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(binary)], check=True)
            data = binary.read_bytes()
            words = struct.unpack('<'+'I'*(len(data)//4), data)
            output += f'inline constexpr uint32_t {prefix}[] = {{\n'
            for start in range(0, len(words), 8):
                output += '  '+', '.join(f'0x{w:08x}' for w in words[start:start+8])+',\n'
            output += '};\n'
            groups = reflect(words)
            refs = []
            for gi, group in enumerate(groups):
                if not group:
                    refs += ['nullptr']*4
                    continue
                for col in range(4):
                    name = f'{prefix}g{gi}c{col}'
                    values = ', '.join('"'+v[col]+'"' if col == 0 else str(v[col]) for v in group)
                    output += f'inline {"const char*" if col == 0 else "constexpr uint32_t"} {name}[] = {{{values}}};\n'
                    refs.append(name)
            blobs.append('{reinterpret_cast<const uint8_t*>('+prefix+'), sizeof('+prefix+'), '+
                ', '.join(str(len(g)) for g in groups)+', '+', '.join(refs)+'}')
    output += 'inline const FfxShaderBlob blobs[] = {\n'+',\n'.join(blobs)+'\n};\n'
    # The first blob of each effect's permutations.
    output += ''.join(f'inline constexpr uint32_t {name} = {start};\n' for name, start in starts)
    output += '}\n'
    if check:
        if TARGET.read_text() != output:
            raise SystemExit('FSR 3.1 embedded shaders differ; regenerate with this glslang version')
    else:
        # Unix line endings on every host: the header is compared byte for
        # byte by --check and by Git.
        TARGET.write_text(output, newline='\n')
