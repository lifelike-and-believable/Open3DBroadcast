import zipfile
import os.path
import os
import sys
import json
import re
import subprocess
import winreg
import shutil

"""
Creates a zip file release for Open3DStream.

The resulting zip is structured with Unreal Engine version folders at the top level:
  UE_5.4/Plugins/Open3DStream/
  UE_5.5/Plugins/Open3DStream/
  lib/
  include/

This makes it easy to extract the appropriate version folder to a project root,
and the Plugins/Open3DStream folder will automatically merge with the project's
Plugins directory.
"""

def get_version():
    exp = re.compile(r'O3DS_VERSION_TAG\s+"([0-9.]+)"')
    with open('CMakeLists.txt') as fp:
        for line in fp:
            ret = exp.search(line)
            if ret:
                return ret.group(1)
    return None
    
    

def build_ue(plugin, ue_base, version, ue_install_path):

    out = ue_base + "\\" + version + "\\Open3dStream"

    exe = rf"{ue_install_path}\Engine\Build\BatchFiles\RunUAT.bat"

    if not os.path.isfile(exe) :      
        raise RuntimeError("Could not find unreal: " + version)
    
    cmd = [exe,
            "BuildPlugin",
            "-Plugin=" + plugin,
            "-TargetPlatforms=Win64",
            "-Package=" + out,
            "-Rocket",
            "-VS2022"
            ]

    try:
        pr = subprocess.Popen(cmd, shell=True)
        pr.wait()

        if pr.returncode != 0:
            raise RuntimeError("Plugin Build Failed")
    except Exception as ex:
        print(ex)


def remove_directory(base):
    # Safely remove a directory tree if it exists
    if not os.path.isdir(base):
        return
    try:
        shutil.rmtree(base)
        print("Removed directory: " + base)
    except Exception as e:
        print(f"Failed to remove directory {base}: {e}")

version_input = get_version()
if version_input is None:
    raise RuntimeError("Could not determine version")
    
print("Packaging version: " + version_input)

version  = "Open3DStream_" + version_input
cwd      = os.path.abspath('')
out_root = "Open3DStream"
outzip   = os.path.join(cwd, version + ".zip")
out_dir  = os.path.abspath('usr')

# Unreal    
unreal_src_plugin = os.path.abspath("D:/P4_PD/o3ds/plugins/Open3DStream/Open3DStream.uplugin")
# Place Unreal plugin under UE_X.X/Plugins/Open3DStream in the zip
unreal_dst_base = out_dir
unreal_stage_base = os.path.join(cwd, "unrealstage")

ue_registry_prefix_path = r'SOFTWARE\EpicGames\Unreal Engine'
ue_registry_prefix_key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, ue_registry_prefix_path, 0, winreg.KEY_READ)
ue_version_index = 0
while True:
    try:
        ue_version = winreg.EnumKey(ue_registry_prefix_key, ue_version_index)
    except OSError as e:
        if "[WinError 259]" in str(e):
            break
        raise e
       
    print(ue_version)
    if ue_version == "4.26":
        ue_version_index = ue_version_index + 1
        continue
    ue_version_key = winreg.OpenKey(ue_registry_prefix_key, ue_version, 0, winreg.KEY_READ)
    ue_path, _ = winreg.QueryValueEx(ue_version_key, "InstalledDirectory")
    build_ue(unreal_src_plugin, unreal_stage_base, ue_version, ue_path)
    
    print(f"Unreal path: {ue_path}")

    # For packaging we want UE_X.X/Plugins/Open3DStream layout
    version_folder = f"UE_{ue_version}"
    dst_plugin_dir = os.path.join(unreal_dst_base, version_folder, "Plugins", "Open3DStream")
    remove_directory(dst_plugin_dir)
    
    ue_host_project_dir = os.path.join(unreal_stage_base, ue_version, "Open3DStream", "HostProject")
    if os.path.isdir(ue_host_project_dir):
        # Project was build as a "host project", copy it to the location
        ue_built_plugin = os.path.join(ue_host_project_dir, "Plugins", "Open3DStream")
    else:
        ue_built_plugin = os.path.join(unreal_stage_base, ue_version, "Open3DStream")
    
    if not os.path.isfile(os.path.join(ue_built_plugin, "Open3DStream.uplugin")):
        raise RuntimeError("Could not find plugin: " + ue_built_plugin)

    # Copy the built plugin into UE_X.X/Plugins/Open3DStream/
    shutil.copytree(ue_built_plugin, dst_plugin_dir)
    
    # libs
    unreal_lib_dir = os.path.join(dst_plugin_dir, "lib")
    print(unreal_lib_dir)
    if not os.path.isdir(unreal_lib_dir):
        os.mkdir(unreal_lib_dir)
                    
    usr_lib_dir = os.path.join(out_dir, "lib")
    for libfile in os.listdir(usr_lib_dir):
        if libfile.endswith(".lib"):
            name = libfile[:-4]            
            if name.endswith("d"):
                release_name = name[:-1] + ".lib"
                if os.path.isfile(os.path.join(out_dir, "lib", release_name)):
                    continue
            print(libfile)
            shutil.copyfile(os.path.join(out_dir, "lib", libfile), os.path.join(unreal_lib_dir, libfile))
        
    # includes
    unreal_include_dir = os.path.join(dst_plugin_dir, "lib", "include")               
    usr_include_dir = os.path.join(out_dir, "include")
    shutil.copytree(usr_include_dir, unreal_include_dir)
            
    ue_version_index = ue_version_index + 1
            


fp = zipfile.ZipFile(outzip, "w", zipfile.ZIP_DEFLATED)


for d, _, f in os.walk(out_dir):
    base = d[len(out_dir)+1:]
    for each_file in f:
        src = os.path.join(d, each_file)
        out = os.path.join(base, each_file)
        print(out)
        fp.write(src, out)
fp.close()

#installer

nsis = "C:/Program Files (x86)/NSIS/makensis.exe"
if not os.path.isfile(nsis):
    raise RuntimeError("Could not find NSIS: " + str(nsis))
    
nsis = '"' + nsis.replace("/", "\\") + '"'
nsi_script = os.path.join(os.path.split(__file__)[0], 'o3ds.nsi')

cmd = nsis + ' /DOUT_FILE=' + version + '.exe /DFILE_VERSION=' + version + ' ' + nsi_script

if subprocess.run(cmd).returncode != 0:
    raise RuntimeError("NSI Error")
    
if not os.path.isfile(version + ".exe"):
    raise RuntimeError("NISI did not produce: " + version + ".exe")
    