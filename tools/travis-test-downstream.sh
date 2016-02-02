#!/bin/bash

set -ex

SRCDIR=$PWD

mkdir workdir || true
cd workdir

wget -c http://repo.continuum.io/miniconda/Miniconda3-latest-Linux-x86_64.sh -O miniconda.sh
rm -rf $PWD/miniconda
bash miniconda.sh -b -p $PWD/miniconda
export PATH=$PWD/miniconda/bin:$PATH

conda install -y scipy astropy scikit-learn pandas
# Apparently 'conda uninstall' ignores dependencies. Handy for us, but if they
# ever fit it then we might have a small problem :-)
conda uninstall -y numpy

pip install -v -U --force-reinstall --ignore-installed $SRCDIR

conda info
pip freeze

python -c 'import numpy; print(numpy.__version__)'

banner() {
    cat <<EOF


################################################################
# $1
################################################################


EOF
}

# FIXME: collect errors and report all at end

banner scipy
python -c "import scipy; scipy.test()"

banner astropy
python -c "import astropy; astropy.test()"

banner sklearn
nosetests -v sklearn

banner pandas
nosetests -v pandas
