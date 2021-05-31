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
		DEFERRED,
		DEFERRED_ALPHA
	};

	enum eLightMode {
		SINGLE,
		MULTI,
		BRUH
	};

	enum eLightEq {
		PHONG,
		DIRECT_LAMB,
		DIRECT_BURLEY,
		NO_EQ
	};

	class Prefab;
	class Material;
	class RenderCall;

	class SSAO
	{
	public:
		int samples;
		float intensity;
		bool plus;
		float bias;
		bool blur;

		FBO* ssao_fbo;
		FBO* ssao_blur_fbo;

		std::vector<Vector3> points;

		SSAO(int points_num, bool plus);
		Texture* apply(Texture* normal_buffer, Texture* depth_buffer, Camera* camera);
	};

	class Renderer
	{

	public:
		static const int max_lights = 100; //Setting the maximum light number to 10

		eRenderMode render_mode;
		eLightMode light_mode;
		eLightEq light_eq;

		bool pcf;
		bool depth_viewport;
		bool dithering;
		bool show_gbuffers;
		bool show_omr; // to show only AO in material properties
		bool activate_ssao; // to activate generating AO instead of using the texture

		int depth_light;
		int shadow_count; //counter for shadows.

		bool hdr_active;
		float hdr_scale;
		float hdr_average_lum;
		float hdr_white_balance;
		float hdr_gamma;

		Renderer();

		std::vector<RenderCall> calls;
		std::vector<LightEntity*> lights;
		LightEntity* directional_light;

		FBO* atlas;
		FBO* gbuffers_fbo;
		FBO* illumination_fbo;
		SSAO* ssao;

		//update the light viewproj matrix and parameters
		void updateLight(LightEntity* light, Camera* camera);

		void renderToFBO(Scene* scene, Camera* camera);

		//renders several elements of the scene
		void renderScene(Scene* scene, Camera* camera);

		//to render a whole prefab (with all its nodes)
		void getCallsFromPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void getCallsFromNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44& model, Mesh* mesh, GTR::Material* material, Camera* camera, Scene* scene);

		//render the shadowmap
		void renderMeshWithMaterialShadow(const Matrix44& model, Mesh* mesh, GTR::Material* material, LightEntity* light);

		//to create the shadowmaps
		void shadowMapping(LightEntity* light, Camera* camera);
		void renderToAtlas(Camera* camera);
		void renderAtlas();

		//different renders for the different light_modes
		void renderMultiPass(Mesh* mesh, Material* material, Shader* shader);
		void renderMultiPassSphere(Shader* sh, Camera* camera);
		void renderSinglePass(Shader* shader, Mesh* mesh);

		//renderers
		void renderForward(std::vector<RenderCall> calls, Camera* camera, Scene* scene);
		void renderDeferred(std::vector<RenderCall> calls, Camera* camera, Scene* scene);
		void passDeferredUniforms(Shader* sh, bool first_pass, Camera* camera, Scene* scene, int& w, int& h);

		//
		void renderGBuffers(std::vector<RenderCall> calls, Camera* camera, Scene* scene, int& w, int& h);
		void showGbuffers(FBO* gbuffers_fbo, Camera* camera);
	};

	Texture* CubemapFromHDRE(const char* filename);
};