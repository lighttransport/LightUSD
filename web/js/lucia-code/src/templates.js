const header = `#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)
`;

const material = (name, color, roughness = 0.35, metallic = 0.1) => `
    def Material "${name}"
    {
        token outputs:surface.connect = </World/Looks/${name}/Shader.outputs:surface>
        def Shader "Shader"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = ${color}
            float inputs:metallic = ${metallic}
            float inputs:roughness = ${roughness}
            token outputs:surface
        }
    }
`;

const cameraAndLights = `
    def Camera "Camera"
    {
        float focalLength = 42
        float3 xformOp:translate = (7, 5, 8)
        float3 xformOp:rotateXYZ = (-18, 38, 0)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
    }
    def DistantLight "KeyLight"
    {
        color3f inputs:color = (1, 0.88, 0.75)
        float inputs:intensity = 2.5
        float3 xformOp:rotateXYZ = (-35, 45, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
    def SphereLight "FillLight"
    {
        float inputs:intensity = 600
        float inputs:radius = 1
        float3 xformOp:translate = (-4, 3, 3)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
`;

export function templateUSDA(kind = 'product') {
  if (kind === 'empty') return `${header}\ndef Xform "World" {}\n`;
  if (kind === 'room') return `${header}
def Xform "World"
{
${cameraAndLights}
    def Cube "Floor" { double size = 8 float3 xformOp:scale = (1, .03, 1) uniform token[] xformOpOrder = ["xformOp:scale"] }
    def Cube "BackWall" { double size = 8 float3 xformOp:translate = (0, 4, -4) float3 xformOp:scale = (1, 1, .03) uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"] }
    def Cube "SideWall" { double size = 8 float3 xformOp:translate = (-4, 4, 0) float3 xformOp:scale = (.03, 1, 1) uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"] }
}
`;
  if (kind === 'materials') {
    const balls = Array.from({ length: 9 }, (_, i) => {
      const x = (i % 3) * 2.4 - 2.4;
      const z = Math.floor(i / 3) * 2.4 - 2.4;
      return `    def Sphere "Ball_${i + 1}" { rel material:binding = </World/Looks/Mat_${i + 1}> float3 xformOp:translate = (${x}, 1, ${z}) uniform token[] xformOpOrder = ["xformOp:translate"] }`;
    }).join('\n');
    const mats = Array.from({ length: 9 }, (_, i) => material(`Mat_${i + 1}`, `(${(i % 3) / 2}, ${Math.floor(i / 3) / 2}, .7)`, (i % 3) / 2, Math.floor(i / 3) / 2)).join('');
    return `${header}\ndef Xform "World"\n{\n${cameraAndLights}\n${balls}\n    def Scope "Looks"\n    {${mats}    }\n}\n`;
  }
  return `${header}
def Xform "World"
{
${cameraAndLights}
    def Cylinder "Turntable"
    {
        double radius = 2.2
        double height = .18
        float3 xformOp:translate = (0, .09, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Sphere "Hero"
    {
        rel material:binding = </World/Looks/HeroMaterial>
        double radius = 1.25
        float3 xformOp:translate = (0, 1.4, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Scope "Looks"
    {${material('HeroMaterial', '(.55, .055, .08)', .22, .85)}    }
}
`;
}
