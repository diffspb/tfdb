project(
  'tfdb',
  'cpp',
  version: '1.0.0',
  default_options: ['cpp_std=c++14', 'warning_level=3'],
  meson_version: '>=1.0.0',
)

threads = dependency('threads')
tfdb_public_inc = include_directories('include')
tfdb_private_inc = include_directories('src')
tfdb_sources = files(
  'src/status.cpp',
  'src/storage_posix.cpp',
  'src/memory_storage.cpp',
  'src/counting_storage.cpp',
  'src/codec.cpp',
  'src/internal_format.cpp',
  'src/ring_store.cpp',
  'src/record_profile.cpp',
  'src/async_writer.cpp',
)

tfdb_library = static_library(
  'tfdb',
  tfdb_sources,
  include_directories: [tfdb_public_inc, tfdb_private_inc],
  dependencies: threads,
  install: true,
)

tfdb_dep = declare_dependency(
  include_directories: tfdb_public_inc,
  link_with: tfdb_library,
  dependencies: threads,
)
meson.override_dependency('tfdb', tfdb_dep)

install_subdir('include/tfdb', install_dir: get_option('includedir'))

tool_targets = []
if get_option('tools')
  foreach tool : ['format', 'inspect', 'verify', 'dump', 'loadgen']
    tool_targets += executable(
      'tfdb_' + tool,
      'tools/tfdb_' + tool + '.cpp',
      include_directories: [include_directories('tools'), tfdb_private_inc],
      dependencies: tfdb_dep,
      install: true,
    )
  endforeach
endif

if get_option('examples')
  executable('tfdb_example_quickstart', 'examples/quickstart.cpp',
             dependencies: tfdb_dep)
  executable('tfdb_example_async_writer', 'examples/async_writer.cpp',
             dependencies: tfdb_dep)
endif

if get_option('tests')
  tfdb_tests = executable('tfdb_tests', 'tests/test_main.cpp',
                          include_directories: tfdb_private_inc,
                          dependencies: tfdb_dep)
  test('tfdb_unit', tfdb_tests)
  executable('tfdb_make_conformance_volume',
             'tests/make_conformance_volume.cpp', dependencies: tfdb_dep)

  if get_option('tools')
    bash = find_program('bash')
    tool_environment = environment()
    tool_environment.set('TOOL_DIR', meson.current_build_dir())
    test(
      'tfdb_tool_smoke',
      bash,
      args: files('tests/tool_smoke.sh'),
      env: tool_environment,
    )
  endif
endif

pkg = import('pkgconfig')
pkg.generate(
  tfdb_library,
  name: 'TFDB',
  filebase: 'tfdb',
  description: 'Telemetry Flash DB C++14 runtime library',
  version: meson.project_version(),
)
