#!/bin/bash

set -ex

NUMPY_SRC=$PWD

# http://stackoverflow.com/a/246128/1925449
TOOLS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

mkdir workdir || true
cd workdir

wget -c http://repo.continuum.io/miniconda/Miniconda3-latest-Linux-x86_64.sh -O miniconda.sh
rm -rf $PWD/miniconda
bash miniconda.sh -b -p $PWD/miniconda
export PATH=$PWD/miniconda/bin:$PATH

conda install -q -y scipy matplotlib astropy scikit-learn pandas statsmodels nose

if [ "$TEST_DOWNSTREAM" = "this" ]; then
    # Apparently 'conda uninstall' ignores dependencies. Handy for us, but if
    # they ever fit it then this will break... :-)
    conda uninstall -q -y numpy
    pip install -q -U --force-reinstall --ignore-installed $SRCDIR
fi

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
python -c "import scipy; scipy.test(verbose=0)" | python $TOOLS_DIR/compress-warnings.py

banner matplotlib
python -c "import matplotlib; matplotlib.test(verbosity=0)" | python $TOOLS_DIR/compress-warnings.py

banner astropy
python -c "import astropy; astropy.test(args='-q')" | python $TOOLS_DIR/compress-warnings.py

banner sklearn
nosetests -q sklearn | python $TOOLS_DIR/compress-warnings.py || true

banner pandas
nosetests -q pandas | python $TOOLS_DIR/compress-warnings.py || true

banner statsmodels
python -c "import statsmodels.api as m; sm.test(verbose=0)" | python $TOOLS_DIR/compress-warnings.py
