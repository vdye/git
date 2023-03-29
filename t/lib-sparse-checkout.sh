# Helper functions for setting up and testing an environment that exercises
# sparse-checkout functionality and compatibility.

populate_test_repo () {
	echo a >a &&
	echo "after deep" >e &&
	echo "after folder1" >g &&
	echo "after x" >z &&
	mkdir -p folder1 folder2 deep before x &&
	echo "before deep" >before/a &&
	echo "before deep again" >before/b &&
	mkdir -p deep/deeper1 deep/deeper2 deep/before deep/later &&
	mkdir -p deep/deeper1/deepest &&
	mkdir -p deep/deeper1/deepest2 &&
	mkdir -p deep/deeper1/deepest3 &&
	echo "after deeper1" >deep/e &&
	echo "after deepest" >deep/deeper1/e &&
	cp a folder1 &&
	cp a folder2 &&
	cp a x &&
	cp a deep &&
	cp a deep/before &&
	cp a deep/deeper1 &&
	cp a deep/deeper2 &&
	cp a deep/later &&
	cp a deep/deeper1/deepest &&
	cp a deep/deeper1/deepest2 &&
	cp a deep/deeper1/deepest3 &&
	cp -r deep/deeper1/ deep/deeper2 &&
	mkdir -p deep/deeper1/0 &&
	mkdir -p deep/deeper1/0/0 &&
	touch deep/deeper1/0/1 &&
	touch deep/deeper1/0/0/0 &&
	>folder1- &&
	>folder1.x &&
	>folder10 &&
	cp -r deep/deeper1/0 folder1 &&
	cp -r deep/deeper1/0 folder2 &&
	echo >>folder1/0/0/0 &&
	echo >>folder2/0/1 &&
	git add . &&
	git commit -m "initial commit" &&
	git checkout -b base &&
	for dir in folder1 folder2 deep
	do
		git checkout -b update-$dir base &&
		echo "updated $dir" >$dir/a &&
		git commit -a -m "update $dir" || return 1
	done &&

	git checkout -b rename-base base &&
	cat >folder1/larger-content <<-\EOF &&
	matching
	lines
	help
	inexact
	renames
	EOF
	cp folder1/larger-content folder2/ &&
	cp folder1/larger-content deep/deeper1/ &&
	git add . &&
	git commit -m "add interesting rename content" &&

	git checkout -b rename-out-to-out rename-base &&
	mv folder1/a folder2/b &&
	mv folder1/larger-content folder2/edited-content &&
	echo >>folder2/edited-content &&
	echo >>folder2/0/1 &&
	echo stuff >>deep/deeper1/a &&
	git add . &&
	git commit -m "rename folder1/... to folder2/..." &&

	git checkout -b rename-out-to-in rename-base &&
	mv folder1/a deep/deeper1/b &&
	echo more stuff >>deep/deeper1/a &&
	rm folder2/0/1 &&
	mkdir folder2/0/1 &&
	echo >>folder2/0/1/1 &&
	mv folder1/larger-content deep/deeper1/edited-content &&
	echo >>deep/deeper1/edited-content &&
	git add . &&
	git commit -m "rename folder1/... to deep/deeper1/..." &&

	git checkout -b rename-in-to-out rename-base &&
	mv deep/deeper1/a folder1/b &&
	echo >>folder2/0/1 &&
	rm -rf folder1/0/0 &&
	echo >>folder1/0/0 &&
	mv deep/deeper1/larger-content folder1/edited-content &&
	echo >>folder1/edited-content &&
	git add . &&
	git commit -m "rename deep/deeper1/... to folder1/..." &&

	git checkout -b df-conflict-1 base &&
	rm -rf folder1 &&
	echo content >folder1 &&
	git add . &&
	git commit -m "dir to file" &&

	git checkout -b df-conflict-2 base &&
	rm -rf folder2 &&
	echo content >folder2 &&
	git add . &&
	git commit -m "dir to file" &&

	git checkout -b fd-conflict base &&
	rm a &&
	mkdir a &&
	echo content >a/a &&
	git add . &&
	git commit -m "file to dir" &&

	for side in left right
	do
		git checkout -b merge-$side base &&
		echo $side >>deep/deeper2/a &&
		echo $side >>folder1/a &&
		echo $side >>folder2/a &&
		git add . &&
		git commit -m "$side" || return 1
	done &&

	git checkout -b deepest base &&
	echo "updated deepest" >deep/deeper1/deepest/a &&
	echo "updated deepest2" >deep/deeper1/deepest2/a &&
	echo "updated deepest3" >deep/deeper1/deepest3/a &&
	git commit -a -m "update deepest" &&

	git checkout -f base &&
	git reset --hard
}
