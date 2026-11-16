suites += {
  'name' : 'local-overlay-store',
  'deps' : [],
  'tests' : [
    'check-post-init.sh',
    'redundant-add.sh',
    'build.sh',
    'bad-uris.sh',
    'add-lower.sh',
    'delete-refs.sh',
    'delete-duplicate.sh',
    'gc.sh',
    'verify.sh',
    'optimise.sh',
    'stale-file-handle.sh',
  ],
  'workdir' : meson.current_source_dir(),
}

suites += {
  'name' : 'local-overlay-store-gc-registration-order',
  'deps' : [],
  'tests' : [ 'gc.sh' ],
  'env' : {'NIX_CONFIG' : 'extra-experimental-features = gc-registration-order'},
  'workdir' : meson.current_source_dir(),
}
