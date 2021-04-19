#!/bin/sh

test_description="Test git odb--daemon"

. ./perf-lib.sh

test_perf_large_repo
test_checkout_worktree

test_odb_daemon_suite() {
	test_perf "status ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE status >/dev/null
	'
	test_perf "status ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE status >/dev/null
	'
	test_perf "status ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE status >/dev/null
	'
	test_perf "status ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE status >/dev/null
	'

	test_perf "diff ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE diff HEAD~1 >/dev/null
	'
	test_perf "diff ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE diff HEAD~1 >/dev/null
	'
	test_perf "diff ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE diff HEAD~1 >/dev/null
	'
	test_perf "diff ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE diff HEAD~1 >/dev/null
	'

	test_perf "log -200 ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE log -200 >/dev/null
	'
	test_perf "log -200 ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE log -200 >/dev/null
	'
	test_perf "log -200 ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE log -200 >/dev/null
	'
	test_perf "log -200 ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE log -200 >/dev/null
	'

	test_perf "rev-list HEAD^{tree} ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD^{tree} >/dev/null
	'
	test_perf "rev-list HEAD^{tree} ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD^{tree} >/dev/null
	'
	test_perf "rev-list HEAD^{tree} ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD^{tree} >/dev/null
	'
	test_perf "rev-list HEAD^{tree} ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD^{tree} >/dev/null
	'

	test_perf "rev-list HEAD ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD >/dev/null
	'
	test_perf "rev-list HEAD ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD >/dev/null
	'
	test_perf "rev-list HEAD ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD >/dev/null
	'
	test_perf "rev-list HEAD ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects HEAD >/dev/null
	'

	test_perf "rev-list --all ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects --all >/dev/null
	'
	test_perf "rev-list --all ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects --all >/dev/null
	'
	test_perf "rev-list --all ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects --all >/dev/null
	'
	test_perf "rev-list --all ($ENABLE)" '
		git -c core.useodboveripc=$ENABLE rev-list --objects --all >/dev/null
	'
}

ENABLE=0
test_odb_daemon_suite

test_expect_success "Start odb--daemon" '
	(git odb--daemon --run &)
'

ENABLE=1
test_odb_daemon_suite

test_expect_success "Stop odb--daemon" '
	git odb--daemon --stop
'

test_done
