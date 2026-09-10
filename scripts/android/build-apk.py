#!/usr/bin/env python3
"""Package the SDL3 native sample as a local debug APK without Gradle."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk', type=Path, default=root / '_work/deps/android/sdk')
parser.add_argument('--ndk', type=Path, default=root / '_work/deps/android/ndk-r29')
parser.add_argument('--deps', type=Path, default=root / '_work/deps/android')
parser.add_argument('--build', type=Path, default=root / '_work/build/android')
parser.add_argument('--validation-layer', type=Path, help='Optional arm64 Khronos validation .so to include in this debug APK')
parser.add_argument('--output', type=Path, default=root / '_work/packages/android')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
sdk = args.sdk.resolve()
bt = sdk / 'build-tools/35.0.0'
android_jar = sdk / 'platforms/android-35/android.jar'
java_home = Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else Path(shutil.which('javac')).resolve().parent.parent
env = dict(os.environ, JAVA_HOME=str(java_home))
def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True, env=env)
compiler = next(args.ndk.glob('toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang++'))
run(compiler, '-std=c++17', '-shared', '-fPIC', '-O2', '-static-libstdc++',
    '-I' + str(root / 'public/include'), root / 'samples/android/main.cpp', '-ldl', '-o', out / 'libmain.so')
classes = out / 'classes'
if classes.exists():
    shutil.rmtree(classes)
classes.mkdir()
sources = list((args.deps / 'sources/sdl3/android-project/app/src/main/java').rglob('*.java'))
sources += list((root / 'samples/android/java').rglob('*.java'))
run(java_home / 'bin/javac', '-source', '8', '-target', '8', '-bootclasspath', android_jar,
    '-d', classes, *sources)
run(java_home / 'bin/jar', 'cf', out / 'classes.jar', '-C', classes, '.')
run(bt / 'd8', '--lib', android_jar, '--min-api', '29', '--output', out, out / 'classes.jar')
apk = out / 'sample-unsigned.apk'
run(bt / 'aapt2', 'link', '-I', android_jar, '--manifest', root / 'samples/android/AndroidManifest.xml', '-o', apk)
with zipfile.ZipFile(apk, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.write(out / 'classes.dex', 'classes.dex')
    for source, name in [(root / 'LICENSE-MIT', 'Remix.txt'),
        (root / 'third_party/imgui/LICENSE.txt', 'ImGui.txt'),
        (root / 'third_party/image/LICENSES.txt', 'ImageDecoders.txt'),
        (root / 'third_party/nrd/LICENSE.txt', 'NRD.txt'),
        (root / 'third_party/nrd/External/MathLib/LICENSE.txt', 'MathLib.txt'),
        (root / 'third_party/nrd/External/ShaderMake/LICENSE.txt', 'ShaderMake.txt'),
        (root / 'third_party/nrd/External/sse2neon/LICENSE', 'sse2neon.txt'),
        (args.deps / 'sources/sdl3/LICENSE.txt', 'SDL3.txt')]:
        archive.write(source, 'assets/licenses/' + name)
    if args.validation_layer:
        archive.write(args.validation_layer, 'lib/arm64-v8a/libVkLayer_khronos_validation.so')
    for source, name in [(out / 'libmain.so', 'libmain.so'),
        (args.build / 'src/native/libremix.so', 'libremix.so'),
        (args.deps / 'prefix/lib/libSDL3.so', 'libSDL3.so')]:
        archive.write(source, 'lib/arm64-v8a/' + name)
run(bt / 'zipalign', '-f', '-p', '4', apk, out / 'sample-aligned.apk')
key = root / '_work/deps/android/sample-debug.keystore'
if not key.exists():
    key.parent.mkdir(parents=True, exist_ok=True)
    run(java_home / 'bin/keytool', '-genkeypair', '-keystore', key, '-storepass', 'android',
        '-alias', 'androiddebugkey', '-keypass', 'android', '-keyalg', 'RSA', '-keysize', '2048',
        '-validity', '3650', '-dname', 'CN=Android Debug,O=Remix Sample,C=US')
run(bt / 'apksigner', 'sign', '--ks', key, '--ks-pass', 'pass:android', '--out',
    out / 'remix-sample.apk', out / 'sample-aligned.apk')
run(bt / 'apksigner', 'verify', out / 'remix-sample.apk')
print(out / 'remix-sample.apk')
