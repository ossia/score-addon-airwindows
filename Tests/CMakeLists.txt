# Metadata inference: probes the airwindows effects directly, no score stack at all.
score_add_test(test_airwindows_metadata
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/MetadataTest.cpp"
  LIBS score_addon_airwindows_core)

# Control construction and the format 1 -> 2 conversion. The process model is
# compiled into the test rather than linked from the plug-in: score plug-ins are
# built with hidden visibility, so Airwindows::ProcessModel is not exported.
score_add_test(test_airwindows_migration
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/MigrationTest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Airwindows/ProcessModel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Airwindows/ProcessModelSerialization.cpp"
  PLUGINS score_lib_process score_plugin_media score_plugin_engine
  LIBS score_addon_airwindows_core)
