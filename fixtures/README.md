# fixtures/

Test fixtures used by examples, smoke tests, and the IDE run configurations.

## `demo-pack/`

A small, multi-file definition pack laid out per `docs/11-definition-format.md`:

```
demo-pack/
├── pack.toml            # metadata + [[identification]]
├── axes.toml            # [[axis]] (RPM, load)
├── scalings.toml        # [[scaling]] (rpm_x1, load_x001, afr_x01, boost_kpa)
└── tables/
    ├── fuel.toml        # primary_open_loop_fuel
    └── boost.toml       # boost_target_high_octane, egr_duty_cycle
```

## `demo.stune/`

A SubuwuTuner project that references `../demo-pack/` and contains a 1024-byte
synthetic source ROM. Created by running `scripts/gen-demo-fixture.py` to seed
`source.bin`, then `subuwutuner-cli project-new` to wrap it in a project. The
project is committed; running the IDE's "GUI (demo)" configuration points at
this directory.

If editing the demo via the GUI or `project-edit` dirties `working.bin` /
`edits.toml` and you want a clean slate, regenerate from scratch:

```bash
rm -rf fixtures/demo.stune
python scripts/gen-demo-fixture.py
build/win-mingw/bin/subuwutuner-cli.exe project-new \
    --source fixtures/demo.stune.source.bin \
    --def fixtures/demo-pack \
    --name "Demo (synthetic)" \
    fixtures/demo.stune
```

(Or `git checkout fixtures/demo.stune/`, which is the quicker reset.)

## `demo-knock-log.csv`

A small synthetic per-cylinder knock datalog used to smoke-test the v1.x
knock dashboard (`docs/05-improvements.md` §11). Represents a third-gear
WOT pull where cyl 1 picks up persistent knock retard and cyls 3-4 stay
clean. Run against it with:

```bash
build/win-mingw/bin/subuwutuner-cli.exe knock-snapshot \
    --log fixtures/demo-knock-log.csv \
    --rpm-col rpm --load-col load \
    --flkc-cols flkc1,flkc2,flkc3,flkc4 \
    --fbkc-cols fbkc1,fbkc2,fbkc3,fbkc4 \
    --sample-rate-hz 5 --window-seconds 60
```

Expected output: cyl 1 stands out as the knocker (FLKC mean ≈ -2.88,
6 events, ≈ -1.78 below all-cyl mean). Same file is a reasonable smoke
input for the GUI panel under **View → Knock dashboard (preview)**.

## Private fixtures

`fixtures/private/` and `fixtures/roms/` are gitignored. Drop user-owned ROM
dumps (your own car, forum-shared stock images, etc.) there for local
development. Anything legally-obtained and not redistributable lives here.
