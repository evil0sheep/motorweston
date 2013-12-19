MotorWeston
===========

MotorWeston is a fork of the [Weston](http://wayland.freedesktop.org/) compositor which is being modified to explore truly 3D windowing, both by bringing 2D windows into a 3D workspace with the help of an Oculus Rift HMD and a Razer Hydra 6DOF mouse, and also by providing a mechanism for applications to request a 3D interface context in the 3D workspace in the same way that a traditional display server allows applications to request a 2D interface context in the 2D workspace. A set of slides explaining the project are available [here](https://docs.google.com/presentation/d/1okL5quA3wzd9oFmWkhbUjWEU89-YJKB3lOPBnHoaE28/edit?usp=sharing).

This is part of a broader project on providing a platform for 3D user interface applications which I guess we are calling Motorcar, hence the project name.

Below is the contents of the Weston README at the time of forking.


Weston
======

Weston is the reference implementation of a Wayland compositor, and a
useful compositor in its own right.  Weston has various backends that
lets it run on Linux kernel modesetting and evdev input as well as
under X11.  Weston ships with a few example clients, from simple
clients that demonstrate certain aspects of the protocol to more
complete clients and a simplistic toolkit.  There is also a quite
capable terminal emulator (weston-terminal) and an toy/example desktop
shell.  Finally, weston also provides integration with the Xorg server
and can pull X clients into the Wayland desktop and act as a X window
manager.

Refer to [http://wayland.freedesktop.org/building.html](http://wayland.freedesktop.org/building.html) for buiding
weston and its dependencies.
