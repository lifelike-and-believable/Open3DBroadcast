import os
import zipfile
import shutil

# This script simulates the packaging zip loop using the repo's package.py layout.
# It creates a temporary usr folder with Plugins/Open3DStream/<version>/ files and then
# runs the same walk-and-zip logic to ensure the zip will contain Plugins/Open3DStream/...

cwd = os.path.abspath('.')
out_dir = os.path.abspath('usr')
version = 'test'
outzip = os.path.join(cwd, f'Open3DStream_{version}.zip')

# Clean and setup
if os.path.isdir(out_dir):
    shutil.rmtree(out_dir)
os.makedirs(out_dir, exist_ok=True)

# Create UE_5.4/Plugins/Open3DStream and UE_5.5/Plugins/Open3DStream folders with dummy uplugins
for ue_version in ['5.4', '5.5']:
    plugin_dir = os.path.join(out_dir, f'UE_{ue_version}', 'Plugins', 'Open3DStream')
    os.makedirs(plugin_dir, exist_ok=True)
    with open(os.path.join(plugin_dir, 'Open3DStream.uplugin'), 'w') as f:
        f.write(f'{{"Name": "Open3DStream", "EngineVersion": "{ue_version}"}}')

# Also add a top-level lib and include to mirror package.py expectations
os.makedirs(os.path.join(out_dir, 'lib'), exist_ok=True)
with open(os.path.join(out_dir, 'lib', 'dummy.lib'), 'w') as f:
    f.write('lib')
os.makedirs(os.path.join(out_dir, 'include'), exist_ok=True)
with open(os.path.join(out_dir, 'include', 'dummy.h'), 'w') as f:
    f.write('// header')

# Create the zip using the same loop as package.py
fp = zipfile.ZipFile(outzip, 'w', zipfile.ZIP_DEFLATED)
for d, _, fns in os.walk(out_dir):
    base = d[len(out_dir)+1:]
    for each_file in fns:
        src = os.path.join(d, each_file)
        out = os.path.join(base, each_file)
        print('Adding', out)
        fp.write(src, out)
fp.close()

# Inspect the zip
with zipfile.ZipFile(outzip, 'r') as z:
    print('\nZip contains:')
    for n in z.namelist():
        print(' ', n)

print('\nTest zip created at', outzip)
