# Translating Building Tools

The mod shows its texts in the game's language. English is built in; every other
language is a plain text file in the `BuildingTools-Languages` folder next to
`dinput8.dll` (in this repository: [`lang/`](lang)).

All translations except English started as drafts written without a native speaker, so
they may contain mistakes or words that differ from the game's own. Corrections are very
welcome, from one word to a whole file.

## Languages

| File | Language | File | Language |
|---|---|---|---|
| `en.txt` | English (template) | `pt-BR.txt` | Português (Brasil) |
| `de.txt` | Deutsch | `ru.txt` | Русский |
| `fr.txt` | Français | `es.txt` | Español |
| `it.txt` | Italiano | `th.txt` | ไทย |
| `ja.txt` | 日本語 | `tr.txt` | Türkçe |
| `ko.txt` | 한국어 | `uk.txt` | Українська |
| `pl.txt` | Polski | `zh-CN.txt` | 简体中文 |
| | | `zh-TW.txt` | 繁體中文 |

These are the languages of the game's own language menu. The mod picks the file from the
game's setting (Settings > Gameplay > Language), or from Steam's language when the game
uses the system language. `Language=` under `[Interface]` in `BuildingTools.ini`, or
Options > Language in the mod's menu, picks one by hand.

## The file format

UTF-8 text, one text per line:

```
# a comment
menu.keys = Klawisze
msg.placed = Postawione kształty: {0}
```

- Keep the key on the left of `=` as it is; translate only the text on the right.
- `{0}`, `{1}` ... are filled in by the mod (numbers, key names, the version). Keep every
  one of them; you can move them where your language needs them.
- Numbers can be anything from 1 to thousands, so write texts that read well with any
  number ("Shapes placed: {0}" rather than "Placed {0} shapes" if your language changes
  the noun with the number).
- `num.decimal` is the decimal mark in distances (`4.0 m` or `4,0 m`).
- The building box rows (`hud.*`) and the info bar words (`bar.*`) have little room:
  keep them short.
- Use the game's own words where the game has them (for example its word for "shape",
  "material" or "On / Off" in that language).
- `menu.translation_note` is the "this translation may contain mistakes" line at the
  bottom of the menu. Leave it empty (`menu.translation_note =`) once a native speaker
  has checked the file.
- A key that is missing from a file shows in English.

## Trying it in the game

1. Edit the file in `BuildingTools-Languages` in the game folder (any editor that saves
   UTF-8, for example Notepad).
2. Open the mod's menu (Home): the file is read again each time the menu opens. Or set
   the language in Options > Language.
3. `Language=test` in the ini shows every English text longer and in `[brackets ~~]`:
   handy to see whether long texts fit and that nothing is left untranslated.

## Sending a correction

Any of these is fine:

- A comment on the Nexus Mods page with the corrected lines.
- A [translation issue](https://github.com/AleksanderWojtal-CC/Enshrouded-BuildingTools/issues/new?template=translation.yml)
  on GitHub, with the file or the lines.
- A pull request changing `lang/<code>.txt`. `./build.sh test` checks that every key is
  there once and that the `{0}` placeholders match English. `python3 tools/lang.py sync`
  puts a file in the order of `en.txt` and adds new keys as comments.

Translators are credited in the changelog.

## For developers

Texts live in [`src/strings.h`](src/strings.h) (key and English). After adding or changing
one there, run `python3 tools/lang.py en` (rewrites `lang/en.txt`) and
`python3 tools/lang.py sync` (adds the new key, commented out in English, to every other
file), then translate it.
