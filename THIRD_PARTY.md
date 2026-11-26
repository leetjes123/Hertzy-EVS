# Third-party software

| Component | Use | Licence | In this repo |
|---|---|---|---|
| [Qt 6.8](https://www.qt.io) (Core, Gui, Network, Qml, Quick, QuickControls2) | UI, linked dynamically | LGPL v3 | no; the packages ship the Qt DLLs (windeployqt) unmodified |
| [pocketfft](https://github.com/mreineck/pocketfft) (C++ header) | FFTs | BSD 3-clause | no; downloaded by CMake at configure time |
| [LUCID Arena SDK](https://thinklucid.com/downloads-hub/) | camera access (Triton2 EVS) | LUCID licence | no; needed to build with camera support. The *full* package ships its runtime DLLs: check LUCID's terms before publishing that package |
| Microsoft Visual C++ runtime | the packages ship it app-local | Microsoft redistributable | no |
| [ffmpeg](https://ffmpeg.org) (optional) | MP4 export, only when bundled with `-Ffmpeg` | GPL / LGPL, depending on the build | no |

Recordings are in the Metavision EVT 3.0 `.raw` format (Prophesee), read and written by our own code (`src/evt3.*`).

The release zips contain these texts in `licenses/` (from this repo's `licenses/` folder). Qt is used unmodified and
linked dynamically, so its DLLs in `app/` can be swapped for another Qt 6.8 build; the Qt 6.8.3 source is at
https://download.qt.io/archive/qt/6.8/6.8.3/single/.
