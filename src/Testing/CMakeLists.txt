ADD_EXECUTABLE(TestReaderSimple TestReaderSimple.cxx)

target_link_libraries(TestReaderSimple
        PUBLIC Nek5000Reader
        PRIVATE 
          VTK::FiltersGeometry
          VTK::InteractionStyle
          VTK::RenderingCore
          VTK::RenderingOpenGL2)
