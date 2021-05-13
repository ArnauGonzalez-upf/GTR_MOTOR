#pragma once
#include "prefab.h"
#include "rendercall.h"
#include "scene.h"
#include "fbo.h"
#include "application.h"

//forward declarations
class Camera;

namespace GTR {

	enum eRenderMode {
		FORWARD,
		SHOW_TEXTURE,
		SHOW_UVS,
		SHOW_NORMALS,
		SHOW_OCCLUSION,
		SHOW_EMISSIVE,
		DEFERRED
	};

	enum eLightMode {
		SINGLE,
		MULTI
	};

	class Prefab;
	class Material;
	class RenderCall;

	class Renderer
	{

	public:
		static const int max_lights = 10; //Setting the maximum light number to 10

		eRenderMode render_mode;
		eLightMode light_mode;
		bool pcf;
		bool depth_viewport;
		int depth_light;

		Renderer();

		std::vector<RenderCall> calls;
		std::vector<LightEntity*> lights;

		FBO* gbuffers_fbo;
		FBO* illumination_fbo;
		bool show_gbuffers;

		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);

		//to render a whole prefab (with all its nodes)
		void getCallsFromPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void getCallsFromNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44& model, Mesh* mesh, GTR::Material* material, Camera* camera);

		//render the shadowmap
		void renderMeshWithMaterialShadow(const Matrix44& model, Mesh* mesh, GTR::Material* material, LightEntity* light);

		//to create the shadowmaps
		void shadowMapping(LightEntity* light, Camera* camera);
		void renderToAtlas(Camera* camera);
		void renderAtlas();

		//different renders for the different light_modes
		void renderMultiPass(Mesh* mesh, Material* material, Shader* shader);
		void renderSinglePass(Shader* shader, Mesh* mesh);

		//renderers
		void renderForward(std::vector<RenderCall> calls, Camera* camera);
		void renderDeferred(std::vector<RenderCall> calls, Camera* camera);
	};

	Texture* CubemapFromHDRE(const char* filename);
};