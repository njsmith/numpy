#!/bin/bash

# Expects:
# - to be run from root of numpy git tree
# - to have TEST_DOWNSTREAM to be set to one of the package names below

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

conda install -q -y nose $TEST_DOWNSTREAM

banner() {
    cat <<EOF


################################################################
# $TEST_DOWNSTREAM tests - $1
################################################################


EOF
}

do_test_internal() {
    pip freeze
    python -c 'import numpy; print(numpy.__version__)'

    case $TEST_DOWNSTREAM in
        scipy)
            python -c "import scipy; scipy.test(verbose=0)"
            ;;
        astropy)
            # args='-q' doesn't seem to actually work... oh well
            python -c "import astropy; astropy.test(args='-q')"
            ;;
        scikit-learn)
            nosetests -q sklearn
            ;;
        pandas)
            nosetests -q pandas
            ;;
        statsmodels)
            python -c "import statsmodels.api as sm; sm.test(verbose=0)"
            ;;
        matplotlib)
            # Doesn't actually work, b/c matplotlib tests require matplotlib
            # test data, which we don't have
            python -c "import matplotlib; matplotlib.test(verbosity=0)"
            ;;
    esac
}

do_test() {
    do_test_internal 2>&1 | python $TOOLS_DIR/compress-warnings.py | tee $1
}

mask_boring_changes() {
    # We don't care about line number changes in exceptions,
    # and we don't care about the cute '***' bar graphs that
    # compress-warnings.py makes
    sed -Ee "/File .*, line/ s/line [0-9]+/line NNN/g" -e 's/^  \*+$/  /' $1
}

################################################################

set +e

banner "OLD numpy"
do_test old.log

# Apparently 'conda uninstall' ignores dependencies. Handy for us, but if
# they ever fix it then this tricky hack will break... :-)
conda uninstall -q -y numpy
pip install -q -U --force-reinstall --ignore-installed $NUMPY_SRC

banner "NEW numpy"
do_test new.log

banner "CHANGES"
# We don't care about changes in line numbers in exception tracebacks
diff -u <(mask_boring_changes old.log) <(mask_boring_changes new.log)

exit 0
