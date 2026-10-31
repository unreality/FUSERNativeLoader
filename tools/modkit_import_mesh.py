# Run in the UE 4.24 modkit (its .uproject needs the PythonScriptPlugin and EditorScriptingUtilities plugins enabled):
#   UE4Editor-Cmd <project>.uproject -run=pythonscript -script="modkit_import_mesh.py <fbx> <scale> <dest folder> <mesh name> <slot>=<material path>[:Material] ..."
# (forward slashes in the paths). A Blender FBX in metres needs scale 0.01.
# Imports an FBX as a static mesh at <dest folder>/<mesh name> and binds each named material slot to a material path.
# A /Game material that doesn't exist in this project is created as an empty placeholder: a MaterialInstanceConstant,
# or a Material with the :Material suffix (the placeholder's class must be the game asset's, which the cooked mesh
# imports it as). Keep placeholders out of the pak, and at run time the reference resolves to the game's own material.
# Example (the pumpkin hat, as a content pak item):
#   modkit_import_mesh.py SM_Hat_PumpkinLantern.fbx 0.01 /Game/CustomContent/PumpkinLantern SM_Hat_PumpkinLantern
#       MI_Hat_Baseball=/Game/Character/Outfit/Hat/Baseball/MI_Hat_Baseball Hair=/Engine/EngineMaterials/WorldGridMaterial
# The five-deck desk (desk_5deck_fbx.py), whose stock materials are Materials:
#   modkit_import_mesh.py SM_WorkspaceDesk_5Deck.fbx 0.01 /Game/Mods/FuserNative SM_WorkspaceDesk_5Deck
#       Worksurface2=/Game/Gameplay/Desk/M_Desk_Unlit:Material Desk_Edge=/Game/Gameplay/Desk/M_BasicEdge:Material
import sys
import unreal

fbx, scale, folder, name = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
bindings = [a.split('=', 1) for a in sys.argv[5:]]
tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


def material(spec):
    path, _, cls = spec.partition(':')
    if eal.does_asset_exist(path):
        return eal.load_asset(path)
    if not path.startswith('/Game/'):
        m = unreal.load_asset(path)
        if not m:
            raise RuntimeError('material %s not found' % path)
        return m
    pkg, asset = path.rsplit('/', 1)
    if cls == 'Material':
        m = tools.create_asset(asset, pkg, unreal.Material, unreal.MaterialFactoryNew())
    else:
        m = tools.create_asset(asset, pkg, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    eal.save_loaded_asset(m)
    return m


mats = [(slot, material(path)) for slot, path in bindings]
dest = folder + '/' + name
if eal.does_asset_exist(dest):
    eal.delete_asset(dest)     # a reimport would keep the old asset's import scale
task = unreal.AssetImportTask()
task.set_editor_property('filename', fbx)
task.set_editor_property('destination_path', folder)
task.set_editor_property('destination_name', name)
task.set_editor_property('automated', True)
task.set_editor_property('replace_existing', True)
task.set_editor_property('save', False)
ui = unreal.FbxImportUI()
ui.set_editor_property('import_mesh', True)
ui.set_editor_property('import_as_skeletal', False)
ui.set_editor_property('import_materials', False)    # slots keep the FBX material names
ui.set_editor_property('import_textures', False)
ui.set_editor_property('import_animations', False)
ui.set_editor_property('mesh_type_to_import', unreal.FBXImportType.FBXIT_STATIC_MESH)
sm = ui.get_editor_property('static_mesh_import_data')
sm.set_editor_property('combine_meshes', True)
sm.set_editor_property('auto_generate_collision', False)
sm.set_editor_property('generate_lightmap_u_vs', False)
sm.set_editor_property('remove_degenerates', True)
sm.set_editor_property('normal_import_method', unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)
# the FBX's vertex colours (the desk's materials read them); a mesh without any gets none
sm.set_editor_property('vertex_color_import_option', unreal.VertexColorImportOption.REPLACE)
sm.set_editor_property('import_uniform_scale', scale)
task.set_editor_property('options', ui)
tools.import_asset_tasks([task])

mesh = eal.load_asset(dest)
for slot, mat in mats:
    i = mesh.get_material_index(slot)
    if i < 0:
        raise RuntimeError('%s has no %s slot' % (name, slot))
    mesh.set_material(i, mat)
# No simple shapes + simple-as-complex: the cooker builds no triangle-mesh collision.
mesh.get_editor_property('body_setup').set_editor_property('collision_trace_flag', unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AS_COMPLEX)
eal.save_loaded_asset(mesh)
b = mesh.get_bounding_box()
unreal.log('MESHIMPORT %s verts=%d materials=%s min=(%.2f %.2f %.2f) max=(%.2f %.2f %.2f)' % (
    dest, unreal.EditorStaticMeshLibrary.get_number_verts(mesh, 0),
    [mesh.get_material(i).get_path_name() for i in range(len(mats))], b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z))
