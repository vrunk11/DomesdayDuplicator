# ddd-requantize — requantising a capture offline, as the capture application would.
#
# Domesday Duplicator - LaserDisc RF sampler
# SPDX-FileCopyrightText: 2026 Simon Inns
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A main() over ddd_capture and nothing else, for the same reason ddd-update is: it
# deliberately does not link Qt, so the requantiser cannot grow a Qt dependency without
# this failing to link. It drives the requantiser the capture application drives, which
# is what makes a capture requantised here identical to one requantised while it was
# taken.

add_executable(ddd-requantize main.cpp)

target_link_libraries(ddd-requantize PRIVATE ddd_capture)

target_compile_options(ddd-requantize PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS ddd-requantize RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
