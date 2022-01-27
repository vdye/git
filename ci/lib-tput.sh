# GitHub Action doesn't set TERM, which is required by tput
export TERM=${TERM:-dumb}
