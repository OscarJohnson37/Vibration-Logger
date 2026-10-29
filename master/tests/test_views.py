import pytest

from logger_master.views import ViewStore

STL = b"solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid t\n"


def test_view_settings_round_trip(tmp_path):
    views = ViewStore(tmp_path)
    assert views.get("9070692f3338") == {}
    views.update("9070692f3338", {"zero": [0.1, -0.2, 0.97], "axis_map": [[0, 1, 0], [-1, 0, 0], [0, 0, 1]]})
    views.update("9070692f3338", {"model_align": [0, 0, 0.7071, 0.7071]})
    v = ViewStore(tmp_path).get("9070692f3338")
    assert v["zero"] == [0.1, -0.2, 0.97]
    assert v["axis_map"] == [[0, 1, 0], [-1, 0, 0], [0, 0, 1]]
    assert v["model_align"] == [0, 0, 0.7071, 0.7071]
    views.update("9070692f3338", {"zero": None})
    assert "zero" not in views.get("9070692f3338")


@pytest.mark.parametrize("changes", [
    {"axis_map": [[1, 1, 0], [0, 1, 0], [0, 0, 1]]},     # not a permutation
    {"axis_map": [[1, 0, 0], [1, 0, 0], [0, 0, 1]]},     # two rows on one axis
    {"zero": [0, 0]},
    {"model_align": [0, 0, 1]},
    {"colour": "red"},
])
def test_invalid_view_changes_rejected(tmp_path, changes):
    with pytest.raises(ValueError):
        ViewStore(tmp_path).update("9070692f3338", changes)


def test_model_store_replace_and_delete(tmp_path):
    views = ViewStore(tmp_path)
    v = views.save_model("9070692f3338", "case.stl", STL)
    assert v["model"] == {"name": "case.stl", "type": "stl", "bytes": len(STL)}
    assert views.model_path("9070692f3338").read_bytes() == STL
    views.save_model("9070692f3338", "case.glb", b"glTF....")
    assert views.model_path("9070692f3338").suffix == ".glb"
    assert not (tmp_path / "9070692f3338.stl").exists()   # replaced, not kept alongside
    views.delete_model("9070692f3338")
    assert views.model_path("9070692f3338") is None and "model" not in views.get("9070692f3338")


def test_bad_ids_and_types_rejected(tmp_path):
    views = ViewStore(tmp_path)
    with pytest.raises(ValueError):
        views.update("../x", {"zero": [0, 0, 1]})
    with pytest.raises(ValueError):
        views.save_model("9070692f3338", "model.exe", b"MZ")
