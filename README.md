This is a plugin source code for the NEK5000 data format, for use with version 5.1[01] of ParaView

How to compile:

  Depending on the optional package you have built into your source (e.g. OSPRay, OIDN,
  VisRTX), you will need some extra env flags to help cmake. In my case, I explicitly set ospray_DIR,
  OpenImageDenoise_DIR, and VisRTX_DIR before running cmake)

  mkdir build

  cd build

  cmake ../

  make

N.B. (January 27, 2023). Thanks to the integration work of Dan Lipsa and Scott Wittenburg at Kitware, the code has now been integrated into VTK. Thus, it will soon be integrated as a native reader in ParaView.
