<img src="tools/branding/art/icon_256.png" width="96" align="left" alt="MiniMe">

# MiniMe Slicer

**MiniMe Slicer** is a fork of Bambu Studio with extra features for FDM printing:

- **Claude chat** inside the slicer: Claude sees the plate, slices, reads layers and filament changes, and changes objects,
  parts, supports and settings (each change can be undone with Ctrl+Z). It uses your own Claude account.
- **Inside filament**: print the inside of an object in one filament and keep colors on the outer layers only.
- **Support tools**: plate zones where tree supports may or may not stand, supports standing on other objects.
- **Cut tool**: curved and shape cuts with connectors; cuts of multi-body models come out clean.
- **Filament looks**: gradient, multi-color, translucent and glow filaments shown on the model.
- **Brazilian filament profiles**: STLFLIX, Voolt3D, GTMax3D, National3D and Masterprint for all Bambu Lab printers.
- **Print with Bambu Connect**: Bambu Lab printers only accept prints from slicers Bambu Lab signs, so MiniMe sends them
  through [Bambu Connect](https://wiki.bambulab.com/en/software/bambu-connect) (install it once).

**Download for Windows:** [latest release](../../releases/latest) (run the installer; Windows may warn that it is unsigned:
More info, Run anyway).

MiniMe Slicer is licensed under the GNU Affero General Public License v3, like Bambu Studio. It is not made or endorsed by
Bambu Lab. The original Bambu Studio README follows.

---

![image](https://user-images.githubusercontent.com/106916061/179006347-497d24c0-9bd6-45b7-8c49-d5cc8ecfe5d7.png)
# BambuStudio
Bambu Studio is a cutting-edge, feature-rich slicing software.  
It contains project-based workflows, systematically optimized slicing algorithms, and an easy-to-use graphic interface, bringing users an incredibly smooth printing experience.

Prebuilt Windows, macOS 64-bit and Linux releases are available through the [github releases page](https://github.com/bambulab/BambuStudio/releases/).

Bambu Studio is based on [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

See the [wiki](https://github.com/bambulab/BambuStudio/wiki) and the [documentation directory](https://github.com/bambulab/BambuStudio/tree/master/doc) for more information.

# What are Bambu Studio's main features?
Key features are:
- Basic slicing features & GCode viewer
- Multiple plates management
- Remote control & monitoring
- Auto-arrange objects
- Auto-orient objects
- Hybrid/Tree/Normal support types, Customized support
- multi-material printing and rich painting tools
- multi-platform (Win/Mac/Linux) support
- Global/Object/Part level slicing parameters

Other major features are:
- Advanced cooling logic controlling fan speed and dynamic print speed
- Auto brim according to mechanical analysis
- Support arc path(G2/G3)
- Support STEP format
- Assembly & explosion view
- Flushing transition-filament into infill/object during filament change

# How to compile
Following platforms are currently supported to compile:
- Windows 64-bit, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Windows-Compile-Guide)
- Mac 64-bit, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Mac-Compile-Guide)
- Linux, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Linux-Compile-Guide)
  - currently we only provide linux appimages on [github releases](https://github.com/bambulab/BambuStudio/releases) for Ubuntu/Fedora, and a [flathub version](https://flathub.org/apps/com.bambulab.BambuStudio) can be used for all the linux platforms

# Report issue
You can add an issue to the [github tracker](https://github.com/bambulab/BambuStudio/issues) if **it isn't already present.**

# License
Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

The bambu networking plugin is based on non-free libraries. It is optional to the Bambu Studio and provides extended networking functionalities for users.
By default, after installing Bambu Studio without the networking plugin, you can initiate printing through the SD card after slicing is completed.

