import sys
import subprocess
import platform
import os

environment_variables = dict()

if platform.system() == 'Linux':
    environment_variables['LD_LIBRARY_PATH'] = './overthrower'
    environment_variables['LD_PRELOAD'] = 'liboverthrower.so'
elif platform.system() == 'Darwin':
    environment_variables['DYLD_FORCE_FLAT_NAMESPACE'] = '1'
    environment_variables['DYLD_INSERT_LIBRARIES'] = './overthrower/overthrower.framework/Versions/Current/overthrower'
else:
    sys.exit(1)

laziness = ['./laziness/laziness']
stubbornness = ['./stubbornness/stubbornness']

subprocess.check_call(laziness)
subprocess.check_call(stubbornness)

for key in environment_variables:
    os.putenv(key, environment_variables[key])

try:
    subprocess.check_call(laziness)
    sys.exit(1)
except subprocess.CalledProcessError as e:
    print('laziness has failed as expected. return code is {}.'.format(e.returncode))
    pass

subprocess.check_call(stubbornness)
