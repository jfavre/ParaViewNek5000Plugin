cmake_minimum_required(VERSION 3.12...3.16 FATAL_ERROR)
paraview_add_plugin(pvNek5000Reader
  REQUIRED_ON_SERVER
  VERSION "1.0"
  MODULES Nek5000Reader
  MODULE_FILES      "${CMAKE_CURRENT_SOURCE_DIR}/Reader/vtk.module")

option(BUILD_TESTING "Build Plugin Testing" OFF)
if (BUILD_TESTING AND BUILD_SHARED_LIBS)
  add_subdirectory(Testing)
endif()
