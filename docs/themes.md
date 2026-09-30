# UI themes

A theme recolours EdgeSlicer beyond light and dark, and can change its fonts, the corners of its
buttons and boxes, the title bar (colour and a banner image) and the 3D view background.

Everything is in the **Themes...** window, right under **Preferences** in the main menu (it is a window of its
own, so Preferences stays quick to open and nothing theme-related is built until you open it):

- **Theme** picks the theme EdgeSlicer starts with. **Default (clean)** is the stock look; it
  cannot be changed or deleted, so **Back to Default** always gets you back to it. **Install...**
  adds a theme from a `.zip`, **Open folder** opens the folder installed themes live in (a theme
  folder can also be copied in by hand), and **Delete** removes one of your own themes.
- Below the list, each part of the selected theme has its own control, with a small picture of the
  window that follows every change: name, author and the look it is built on (light, dark, or
  following the dark mode setting); a colour picker per palette role (**reset** puts a role back to
  stock); a font list for text, headings and buttons, with every installed font plus **From
  file...** for a `.ttf` or `.otf`, and a line of sample text under each list set in the chosen
  font (a theme's own font files are loaded for it too); corner sizes for buttons and boxes; and the title bar banner,
  chosen with a file dialog, with its placement.
- **Save** writes your own themes in place. Default and the themes that come with EdgeSlicer are
  read-only, so saving a change to one asks for a name and makes your own copy; **Save as new...**
  does the same for one of yours. Picked font and image files are copied into the theme's folder.

Picking a theme, installing one, deleting the one in use, or saving a change to it applies the
colours, corner sizes, icons, title bar, 3D view background and Home page at once, with no restart
(windows that are open next to the main one follow too). Fonts are loaded when EdgeSlicer starts, so
a change of fonts shows after a restart: only then does EdgeSlicer ask whether to **Restart now** or
**Later**. Restarting closes EdgeSlicer the way File > Quit does (an unsaved project is offered for
saving, and cancelling that cancels the restart) and starts it again with the same command line.
**Later** leaves a highlighted note on the page saying the fonts wait for a restart, and **Restart to
apply fonts** at the bottom of the page stays available whenever that is so. Every save is a new
change and asks again; picking the same theme twice asks once.
Parts the page has no control for (`overrides`, `home`) are kept as they are when a theme is saved.

EdgeSlicer ships two sample themes with original art: **Ember Forge** (dark, charcoal and oxblood
with bronze) and **Silver Bastion** (light, parchment and navy with gold). Both use the Cinzel font
(SIL Open Font License, `fonts/Cinzel-OFL.txt`).

## Where themes live

| | |
|---|---|
| Shipped | `resources/themes/<folder>/` |
| Installed | `<data dir>/themes/<folder>/` (Windows: `%APPDATA%\EdgeSlicer\themes`) |

An installed theme with the same folder name as a shipped one wins. The folder name is what the
`ui_theme` setting stores; installing from a zip names the folder after the theme's `name`.

## Pack layout

```
my-theme/
  theme.json          required
  fonts/*.ttf|*.otf   optional
  images/*.png|*.jpg  optional
```

A zip holds that folder, or its contents directly. Only `.json .png .jpg .jpeg .bmp .ttf .otf
.txt .md` files are installed, at most 500 files and 64 MB. Every path in `theme.json` is relative
to the pack folder and must stay inside it (no `..`, no drive letters, no absolute paths). A theme
is data only: nothing in it runs.

## theme.json

Everything except `name` is optional; whatever a theme leaves out keeps the stock look.

```json
{
  "name": "Ember Forge",
  "author": "You",
  "version": "1.0",
  "description": "One line for the list.",
  "base": "dark",
  "palette": { "accent": "#B8322A", "window_bg": "#1E1614" },
  "overrides": { "#DFDFDF": "#3B2B25" },
  "fonts": {
    "body":    { "files": ["fonts/Body.ttf"], "face": "Body Face" },
    "heading": { "files": ["fonts/Cinzel-Bold.ttf"], "face": "Cinzel" },
    "button":  { "files": ["fonts/Cinzel-Regular.ttf"], "face": "Cinzel" }
  },
  "shapes": { "button_radius": 2, "box_radius": 2 },
  "titlebar": { "banner": "images/banner.png", "align": "left" },
  "home": { "--warn-bg": "#3D2A14" }
}
```

- **base**: `"light"` or `"dark"`. The theme is laid over that look, so a small palette is
  enough. Without it the theme follows the dark mode setting. On macOS the system appearance
  decides light or dark either way.
- **palette**: `#RRGGBB` per role (below).
- **overrides**: for fine tuning, any stock light colour (a key of the table in
  `src/slic3r/GUI/Widgets/StateColor.cpp`) to a colour. Applied after the palette.
- **fonts**: `face` is the font's family name. `files` are loaded for this session only; a face
  already installed on the computer can be named without files. `body` is all regular text,
  `heading` all bold titles, `button` every button label, the main tabs included. On macOS a font
  file may not load outside the app bundle, in which case install the font on the Mac and name
  its face. Wide display fonts can crowd fixed-size controls, so they suit `heading` and `button`
  better than `body`.
- **shapes**: corner radius in DIP, 0 to 24. `button_radius` for buttons, `box_radius` for inputs,
  combo boxes and cards. Square controls stay square and round ones (dots, circular buttons) stay
  round; pill-shaped buttons take the theme's corners.
- **titlebar.banner**: a PNG, JPG or BMP (at most 8 MB, 8192x1024) drawn behind the title bar's
  menus, title and buttons, scaled to the bar's height. **align**: `left`, `center`, `right`,
  `tile` (repeat across) or `stretch` (fill, ignoring its proportions). Make it fade into
  `titlebar_bg` and keep it low in contrast so the title and buttons stay readable. Windows and
  Linux only; macOS keeps its own title bar and takes `titlebar_bg` for the strip below it.
- **home**: Home tab CSS variables (`resources/web/home/home.css`), over the ones the palette sets.

### Palette roles

| Role | Recolours |
|---|---|
| `window_bg` | window and dialog backgrounds, inputs |
| `panel_bg` | side panels, title strips, tab pages |
| `sidebar_bg` | sidebar panels |
| `text` | main text, input text |
| `text_secondary` | labels, secondary text |
| `text_disabled` | disabled and dimmed text |
| `accent` | the highlight colour: confirm buttons, selected tab, checks, toggles |
| `accent_hover` | accent buttons when hovered |
| `accent_soft` | selected rows, focused inputs |
| `accent_text` | text on accent buttons and on the main tabs |
| `secondary_accent` | the secondary (orange) highlight |
| `button_bg`, `button_hover_bg` | regular buttons |
| `border` | input and combo box borders |
| `separator` | lines and dividers |
| `disabled_bg` | disabled control backgrounds |
| `toggle_track` | switch tracks |
| `error` | error text |
| `tabbar_bg`, `tabbar_hover` | the main tab bar (Home, Prepare, Preview, Device...) |
| `titlebar_bg`, `titlebar_text` | the title bar |
| `canvas_bg`, `canvas_bg_top` | the 3D view: one colour, or a gradient up to `canvas_bg_top` |
| `icon` | the main line colour of the built-in icons |

## What a theme does not reach yet

Colours that are written straight into a screen instead of going through the shared colour table
stay stock: some device pages (FlashForge, Bambu), a few dialogs, and the overlay panels in the 3D
view. Textured (image) panels and buttons are not supported.
