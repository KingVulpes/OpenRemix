# The GPU fixtures link runtime objects to inspect internal preparation status.
# Keep SDK discovery relative to their executable, just as in a deployed build.
if has_dlss
  foreach binary : dlss_binaries
    configure_file(input : meson.project_source_root() / 'third_party/dlss' / dlss_runtime_dir / binary,
      output : binary, copy : true)
  endforeach
endif
