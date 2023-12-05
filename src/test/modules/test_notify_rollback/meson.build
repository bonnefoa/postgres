# Copyright (c) 2022-2023, PostgreSQL Global Development Group

test_notify_rollback_sources = files(
  'test_notify_rollback.c',
)

if host_system == 'windows'
  test_notify_rollback_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_notify_rollback',
    '--FILEDESC', 'test_notify_rollback - send async notification during rollback',])
endif

test_notify_rollback = shared_module('test_notify_rollback',
  test_notify_rollback_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_notify_rollback

tests += {
  'name': 'test_notify_rollback',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_notify_rollback',
    ],
    'regress_args': ['--no-locale', '--encoding=UTF8'],
    'runningcheck': false,
  },
}
