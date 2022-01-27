if test "$GITHUB_ACTIONS" = "true"
then
	CI_TYPE=github-actions
else
	echo "Could not identify CI type" >&2
	env >&2
	exit 1
fi
