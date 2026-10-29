<p align="center">
  <img src="Oshi.svg" width="128" alt="oshi logo">
</p>

<h1 align="center">oshi</h1>

<p align="center">minimal text editor that does it's work...</p>

```sh
make
make install          # ~/.local/bin/oshi
sudo make install     # /usr/local/bin/oshi
```

```sh
oshi some-file.txt
oshi some-folder/     # a file browser: open, create, rename, delete
oshi 
```

In the browser (`?` shows this list):

| key | does |
| --- | --- |
| `up` `down` / `j` `k` | move |
| `enter` / `right` / `l` | open a file or folder |
| `backspace` / `left` / `h` | up a folder |
| `n` / `N` | new file / new folder |
| `r` | rename |
| `d` / `del` | delete (asks, `y` to confirm) |
| `.` | show or hide dotfiles |
| `q` / `ctrl-q` | quit |

`ctrl-q` in a file opened from the browser comes back to the list.

