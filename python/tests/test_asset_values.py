import lightusd


def test_asset_scalar_is_exposed_as_string():
    stage = lightusd.loads(
        '''#usda 1.0
def Shader "Texture" {
    asset inputs:file = @textures/hair_albedo.<UDIM>.png@
}
'''
    )
    shader = stage.prim_at("/Texture")
    assert shader.get("inputs:file") == "textures/hair_albedo.<UDIM>.png"
    assert shader.attribute("inputs:file").get() == "textures/hair_albedo.<UDIM>.png"
