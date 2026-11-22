"""Species from rule files in species/: which growth model, its numbers, and its foliage."""

import dataclasses
import tomllib
from dataclasses import dataclass
from pathlib import Path

import calibrate
import grow_param
import grow_sot
import palm
import pinegrow

FOLDER = Path(__file__).with_name("species")


@dataclass
class Species:
    name: str
    model: str  # sot | rules | pine | palm
    kind: str  # broadleaf | conifer | palm
    params: object
    full: int = 30
    foliage: dict = dataclasses.field(default_factory=dict)
    bark: dict = dataclasses.field(default_factory=dict)

    def grow(self, seed, ages):
        """Skeletons of one tree at each of `ages` years."""
        if self.model == "sot":
            return grow_sot.grow(self.params, seed, set(ages))
        if self.model == "rules":
            return grow_param.grow(self.params, seed, set(ages), full=self.full)
        if self.model == "pine":
            return pinegrow.grow(self.params, seed, set(ages))
        return palm.grow(self.params, seed, set(ages))


def _fields(cls, table):
    known = {f.name: f for f in dataclasses.fields(cls)}
    out = {}
    for k, v in table.items():
        if k not in known:
            raise KeyError(f"{cls.__name__} has no {k}")
        out[k] = tuple(v) if isinstance(v, list) and k != "levels" else v
    return out


def load(name, calibrated=True):
    data = tomllib.loads((FOLDER / f"{name}.toml").read_text())
    model = data["model"]
    growth = dict(data.get("growth", {}))
    if model == "sot":
        params = grow_sot.Params(**_fields(grow_sot.Params, growth))
        if calibrated and "targets" in data:
            params = calibrate.calibrate(params, calibrate.Targets(**data["targets"]),
                                         verbose=False)
    elif model == "rules":
        levels = [grow_param.Level(**lv) for lv in growth.pop("levels")]
        params = grow_param.Params(levels=levels, **_fields(grow_param.Params, growth))
    elif model == "pine":
        params = pinegrow.Params(**_fields(pinegrow.Params, growth))
    else:
        params = palm.Params(**_fields(palm.Params, growth))
    return Species(name, model, data["kind"], params, data.get("full", 30),
                   data.get("foliage", {}), data.get("bark", {}))


def names():
    return sorted(p.stem for p in FOLDER.glob("*.toml"))
