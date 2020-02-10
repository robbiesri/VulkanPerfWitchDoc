#!/user/bin/env python2

# copy layer and PDB (if it exists) to bin - skip, leave it to build process
# copy proper layer into bin folder
# Windows - copy regkey
# Linux - copy to target folder
#   Allow for override destination

import argparse
import platform
import os.path
import sys
import shutil
import json

if os.name == 'nt':
    import _winreg


def linuxLayerPathSearch(layerMode):
    print '    GPUVoyeur - Searching known layer JSON directories for possible install destination'

    searchFolder = layerMode + '_layer.d'
    
    searchPathList = ['/usr/local/etc/vulkan',
                      '/usr/local/share/vulkan',
                      '/etc/vulkan',
                      '/usr/share/vulkan',
                      '$HOME/.local/share/vulkan']

    for basePath in searchPathList:
        searchPath = os.path.join(basePath, searchFolder)

        if os.path.isdir(searchPath):
            json_files = [pos_json for pos_json in os.listdir(searchPath) if pos_json.endswith('.json')]

            if json_files:
                print '    GPUVoyeur - Candidate install path for layer JSON found: ' + searchPath
                return searchPath

    return None



parser = argparse.ArgumentParser(description='Install the GPUVoyeur layer onto your system.')

parser.add_argument('--layerMode', type=str, dest='mode',
                    default='explicit', 
                    help="Choose the layer install type: explicit or implicit (default: explicit).")

parser.add_argument('--layerSearchPath', type=str, dest='searchPath',
                    help="(LINUX ONLY) Specify the layer search path, if in non-traditional location.")

parser.add_argument('--configInstallPath', type=str, dest='configPath',
                    help="Specify the complete PerfHaus.cfg install path (default: ~/VkPerfHaus/PerfHaus.cfg).")


args = parser.parse_args()

print "Starting GPUVoyeur layer installation..."

# Copy layer JSON into the bin folder

layerMode = args.mode
print "    GPUVoyeur - Layer Mode: " + layerMode

if layerMode == "explicit" or layerMode == "exp" or layerMode == "e":
    layerMode = 'explicit'
elif layerMode == "implicit" or layerMode == "imp" or layerMode == "i":
    layerMode = 'implicit'
else:
    sys.exit("Unsupported layer install type, please use either 'explicit' or 'implicit'!")

osStr = platform.system()

osDir = "Linux"
if osStr == "Windows":
    osDir = "windows"

scriptFilePath = os.path.abspath(os.path.realpath(__file__))
toolsPath = os.path.dirname(scriptFilePath)
rootPath = os.path.dirname(toolsPath)

destLayerFilePath = os.path.join(rootPath, 'bin', osDir, 'VkLayer_GPUVoyeur.json')

sourceFileName = "VkLayer_GPUVoyeur_" + layerMode + ".json"
sourceLayerFilePath = os.path.join(rootPath, 'resources', osDir, sourceFileName)

print "    GPUVoyeur - Layer Source: " + sourceLayerFilePath
print "    GPUVoyeur - Layer Dest: " + destLayerFilePath

shutil.copyfile(sourceLayerFilePath, destLayerFilePath)

# OS specific config stuff

if osStr == "Windows":
    
    print "    GPUVoyeur - Adding layer key to registry"

    regPath = r"SOFTWARE\Khronos\Vulkan"
    if layerMode == "implicit":
        regPath = regPath + r"\ImplicitLayers"
    else:
        regPath = regPath + r"\ExplicitLayers"

    _winreg.CreateKey(_winreg.HKEY_LOCAL_MACHINE, regPath)

    regKey = _winreg.OpenKey(_winreg.HKEY_LOCAL_MACHINE, regPath, 0, _winreg.KEY_WRITE)
    _winreg.SetValueEx(regKey, destLayerFilePath, 0, _winreg.REG_DWORD, 0)
    _winreg.CloseKey(regKey)
else:
    
    print "    GPUVoyeur - Copying layer JSON to layer search directory"

    searchPath = None
    if args.searchPath:
        searchPath = args.searchPath
    else:
        searchPath = linuxLayerPathSearch(layerMode)

    if searchPath == None:
        sys.exit("Could not locate path to copy layer JSON to. Perhaps specify with --layerSearchPath?")

    subFolder = layerMode + '_layer.d'
    layerJsonInstallPath = os.path.join(searchPath, subFolder, "VkLayer_GPUVoyeur.json")

    shutil.copyfile(sourceLayerFilePath, layerJsonInstallPath)

    # we have to edit the path to be absolute

    libFilePath = os.path.join(rootPath, 'bin', osDir, 'libGPUVoyeur.so')
    with open(layerJsonInstallPath, 'r+') as f:
        data = json.load(f)
        data['layer']['library_path'] = libFilePath
        f.seek(0)
        json.dump(data, f, indent=4)
        f.truncate()

    print "    GPUVoyeur - Layer JSON copied to " + layerJsonInstallPath

# print "Setting up PerfHaus.cfg..."
# home = os.path.expanduser("~")

# defaultConfigPath = home + "/VkPerfHaus/PerfHaus.cfg"
# sampleConfigPath = os.path.join(rootPath, 'resources', 'sample_PerfHaus.cfg')

# configPath = defaultConfigPath
# if args.configPath:
#     configPath = args.configPath

# print "    GPUVoyeur - Sample Config Source: " + sampleConfigPath
# print "    GPUVoyeur - Config Dest: " + configPath
# shutil.copyfile(sampleConfigPath, configPath)


print "GPUVoyeur layer installation complete!"