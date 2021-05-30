#include "renderer.h"

#include "camera.h"
#include "shader.h"
#include "mesh.h"
#include "texture.h"
#include "prefab.h"
#include "material.h"
#include "utils.h"
#include "scene.h"
#include "extra/hdre.h"
#include "rendercall.h"
#include <iostream>
#include <algorithm>
#include <vector>
#include "math.h"

using namespace GTR;

Renderer::Renderer()
{
	render_mode = eRenderMode::DEFERRED;
	light_mode = eLightMode::MULTI;
	light_eq = eLightEq::DIRECT_BURLEY;
	pcf = false;
	depth_viewport = false;
	dithering = true;
	depth_light = 0;

	gbuffers_fbo = new FBO();
	atlas = NULL;

	show_gbuffers = false;

	activate_ssao = true;
	ssao = new SSAO(64, true);

	show_omr = false;

	hdr_active = true;
	hdr_scale = 1.0;
	hdr_average_lum = 1.5;
	hdr_white_balance = 0.5;
	hdr_gamma = 2.2;

	illumination_fbo = new FBO();
	illumination_fbo->create(Application::instance->window_width, Application::instance->window_height,
			1,            //one textures
			GL_RGB,       //four channels
			GL_HALF_FLOAT,//half float
			true);        //add depth_texture)
}

//renders all the prefab
void Renderer::getCallsFromPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	getCallsFromNode(model, &prefab->root, camera);
}

//renders a node of the prefab and its children
void Renderer::getCallsFromNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera)
{
	if (!node->visible)
		return;

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true) * prefab_model;

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//Create RenderCall
		RenderCall call = RenderCall(node->mesh, node->material, node_model);
		calls.push_back(call);
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		getCallsFromNode(prefab_model, node->children[i], camera);
}

void Renderer::updateLight(LightEntity* light, Camera* camera)
{
	Vector3 pos;
	light->camera->lookAt(light->model.getTranslation(), light->model * Vector3(0, 0, 1), light->model.rotateVector(Vector3(0, 1, 0)));

	switch (light->light_type)
	{
	case POINT: return; break;
	case SPOT:
		light->camera->setPerspective(2 * light->cone_angle, Application::instance->window_width / (float)Application::instance->window_width, 0.1f, light->max_distance);
		break;
	case DIRECTIONAL:
		//pos = camera->eye - (light->model.rotateVector(Vector3(0, 0, 1)) * 1000);
		//light->camera->lookAt(pos, pos + light->model.rotateVector(Vector3(0, 0, 1)), light->model.rotateVector(Vector3(0, 1, 0)));
		light->camera->setOrthographic(-light->area_size, light->area_size, -light->area_size, light->area_size, 0.1f, light->max_distance);

		//Texel in world units (assuming rectangular)
		float grid = (light->camera->right - light->camera->left) / (float)light->shadow_fbo->depth_texture->width;

		//Snap camera X,Y
		light->camera->view_matrix.M[3][0] = round(light->camera->view_matrix.M[3][0] / grid) * grid;
		light->camera->view_matrix.M[3][1] = round(light->camera->view_matrix.M[3][1] / grid) * grid;

		//Update viewproj matrix
		light->camera->viewprojection_matrix = light->camera->view_matrix * light->camera->projection_matrix;
		break;
	}
}

void Renderer::shadowMapping(LightEntity* light, Camera* camera)
{
	updateLight(light, camera);

	//Bind to render inside a texture
	light->shadow_fbo->bind();
	glColorMask(false, false, false, false);
	glClear(GL_DEPTH_BUFFER_BIT);

	for (int i = 0; i < calls.size(); ++i)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(calls[i].model, calls[i].mesh->box);

		//if bounding box is inside the camera frustum then the object is probably visible
		if (light->camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize))
		{
			renderMeshWithMaterialShadow(calls[i].model, calls[i].mesh, calls[i].material, light);
		}
	}

	//disable it to render back to the screen
	light->shadow_fbo->unbind();
	glColorMask(true, true, true, true);
}

void Renderer::renderGBuffers(std::vector<RenderCall> calls, Camera* camera)
{
	if (gbuffers_fbo->fbo_id == 0) {
		Texture* albedo = new Texture(Application::instance->window_width, Application::instance->window_height, GL_RGBA, GL_HALF_FLOAT);
		Texture* normals = new Texture(Application::instance->window_width, Application::instance->window_height, GL_RGBA, GL_UNSIGNED_BYTE);
		Texture* extra = new Texture(Application::instance->window_width, Application::instance->window_height, GL_RGBA, GL_HALF_FLOAT);
		Texture* depth = new Texture(Application::instance->window_width, Application::instance->window_height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, false);

		std::vector<Texture*> text = { albedo,normals,extra };

		gbuffers_fbo->setTextures(text, depth);
	}

	//start rendering inside the gbuffers
	gbuffers_fbo->bind();

	//we clear in several passes so we can control the clear color independently for every gbuffer
	//disable all but the GB0 (and the depth)
	gbuffers_fbo->enableSingleBuffer(0);

	//clear GB0 with the color (and depth)
	Vector3 bg_color = Scene::instance->background_color;
	glClearColor(bg_color.x, bg_color.y, bg_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	//and now enable the second GB to clear it to black
	gbuffers_fbo->enableSingleBuffer(1);
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	//and now enable the second GB to clear it to black
	gbuffers_fbo->enableSingleBuffer(2);
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	//enable all buffers back
	gbuffers_fbo->enableAllBuffers();

	//render everything 
	//Rendering the final scene
	for (int i = 0; i < calls.size(); ++i)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(calls[i].model, calls[i].mesh->box);

		//if bounding box is inside the camera frustum then the object is probably visible
		if (camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize))
		{
			renderMeshWithMaterial(calls[i].model, calls[i].mesh, calls[i].material, camera);
		}
	}

	//stop rendering to the gbuffers
	gbuffers_fbo->unbind();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
}

void GTR::Renderer::renderToFBO(GTR::Scene* scene, Camera* camera)
{
	renderScene(scene, camera);

	//and render the texture into the screen
	Shader* hdr_shader = Shader::Get("hdr");

	hdr_shader->enable();
	hdr_shader->setUniform("u_hdr", hdr_active);

	if (hdr_active)
	{
		hdr_shader->setUniform("u_scale", hdr_scale);
		hdr_shader->setUniform("u_average_lum", hdr_average_lum);
		hdr_shader->setUniform("u_lumwhite2", hdr_white_balance);
		float inv_gamma = 1 / hdr_gamma;
		hdr_shader->setUniform("u_igamma", inv_gamma);
	}
	illumination_fbo->color_textures[0]->toViewport(hdr_shader);


	if (render_mode == DEFERRED && show_gbuffers) {
		showGbuffers(gbuffers_fbo, camera);
	}

	//Render one light camera depth map
	if (lights.size() > 0 && depth_viewport && light_mode == MULTI)
	{
		LightEntity* light = lights[depth_light];
		glViewport(20, 20, Application::instance->window_width / 4, Application::instance->window_height / 4); //Defining a big enough viewport
		Shader* zshader = Shader::Get("depth");
		zshader->enable();

		//Passing uniforms
		if (light->light_type == DIRECTIONAL) { zshader->setUniform("u_cam_type", 0); }
		else { zshader->setUniform("u_cam_type", 1); }
		zshader->setUniform("u_camera_nearfar", Vector2(light->camera->near_plane, light->camera->far_plane));

		light->shadow_fbo->depth_texture->toViewport(zshader);
	}
	if (light_mode == SINGLE && depth_viewport && shadow_count > 0)
		renderAtlas();
}

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	lights.clear(); //Clearing lights vector
	calls.clear(); //Cleaning rendercalls vector
	directional_light = NULL;
	shadow_count = 0; //resetting counter of shadows

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	for (int i = 0; i < scene->entities.size(); ++i)
	{
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible)
			continue;

		//is a prefab!
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if (pent->prefab)
				getCallsFromPrefab(ent->model, pent->prefab, camera);
		}

		if (ent->entity_type == LIGHT)
		{
			LightEntity* light = (GTR::LightEntity*)ent;

			if (light->light_type == DIRECTIONAL)
			{
				render_mode == DEFERRED ? directional_light = light: lights.push_back(light);
				shadow_count++;
			}
			else if (light->lightBounding(camera)) 
			{
				if (light->light_type != POINT)
					shadow_count++;
				lights.push_back(light);
			}
		}
	}

	//Sorting rendercalls
	std::sort(calls.begin(), calls.end());

	//Calculate the shadowmaps
	if (light_mode == MULTI) 
	{
		for (int i = 0; i < lights.size(); ++i) 
		{
			LightEntity* light = lights[i];
			if (light->cast_shadows)
				shadowMapping(light, camera);
		}

		if (render_mode == DEFERRED)
		{
			if (directional_light && directional_light->cast_shadows)
				shadowMapping(directional_light, camera);
		}
	}
	else if (light_mode == SINGLE)
		renderToAtlas(camera);

	if (render_mode == FORWARD)
	{
		illumination_fbo->bind();
		renderForward(calls, camera);
		illumination_fbo->unbind();
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
	}
	else if (render_mode == DEFERRED)
		renderDeferred(calls, camera);
}

void Renderer::renderForward(std::vector<RenderCall> calls, Camera* camera)
{
	Vector3 bg_color = Scene::instance->background_color;
	glClearColor(bg_color.x, bg_color.y, bg_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	//Rendering the final scene
	for (int i = 0; i < calls.size(); ++i)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(calls[i].model, calls[i].mesh->box);

		//if bounding box is inside the camera frustum then the object is probably visible
		if (camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize))
		{
			renderMeshWithMaterial(calls[i].model, calls[i].mesh, calls[i].material, camera);
		}
	}
}

void Renderer::renderDeferred(std::vector<RenderCall> calls, Camera* camera)
{
	int w = Application::instance->window_width;
	int h = Application::instance->window_height;

	renderGBuffers(calls, camera);

	Texture* ao = NULL;
	if (activate_ssao)
		ao = ssao->apply(gbuffers_fbo->color_textures[1], gbuffers_fbo->depth_texture, camera);

	//we need a fullscreen quad
	Mesh* quad = Mesh::getQuad();

	//start rendering inside the gbuffers
	illumination_fbo->bind();

	glClearColor(0.0, 0.0, 0.0, 0.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	//we need a shader specially for this task, lets call it "deferred"
	Shader* sh = NULL;
	sh = Shader::Get("deferred_multi");
	sh->enable();

	passDeferredUniforms(sh, true, camera, w, h);

	if (activate_ssao)
	{
		sh->setUniform("u_ao_texture", ao, 5);
		sh->setUniform("u_ao_factor", ssao->intensity);
	}
		
	if (directional_light) {
		sh->setUniform("u_pcf", pcf);
		directional_light->uploadLightParams(sh, true, hdr_gamma);
	}
	else { sh->setUniform("u_light_eq", 5); }

	quad->render(GL_TRIANGLES);

	sh->disable();

	//renderMultiPass(quad, NULL, sh);
	sh = Shader::Get("deferred_ws");

	sh->enable();

	passDeferredUniforms(sh, true, camera, w, h);
	renderMultiPassSphere(sh, camera);

	sh->disable();

	//disable depth test and blend!!
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	illumination_fbo->unbind();

	//gbuffers_fbo->color_textures[1]->copyTo(illumination_fbo->color_textures[0], Shader::getDefaultShader("screen_depth"));

	/*if (!dithering) {
		render_mode = FORWARD;
		light_mode = MULTI;
		gbuffers_fbo->depth_texture->copyTo(illumination_fbo->depth_texture, NULL);

		lights.push_back(directional_light);
		for (int i = 0; i < calls.size(); ++i)
		{
			if (calls[i].material->alpha_mode != BLEND)
				continue;
			//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
			BoundingBox world_bounding = transformBoundingBox(calls[i].model, calls[i].mesh->box);
			//if bounding box is inside the camera frustum then the object is probably visible
			if (camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize))
				renderMeshWithMaterial(calls[i].model, calls[i].mesh, calls[i].material, camera);
		}
		render_mode = DEFERRED;
	}*/
}

void Renderer::renderMeshWithMaterialShadow(const Matrix44& model, Mesh* mesh, GTR::Material* material, LightEntity* light)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Texture* texture = NULL;
	Shader* shader = NULL;
	shader = Shader::Get("shadowmap");
	assert(glGetError() == GL_NO_ERROR);

	texture = material->color_texture.texture;
	if (!texture)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture

	//no shader? then nothing to render; material with blending properties? then nothing to render
	if (!shader || material->alpha_mode == GTR::eAlphaMode::BLEND)
		return;

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);
	
	shader->enable();

	Matrix44 shadow_proj = light->camera->viewprojection_matrix;
	shader->setUniform("u_viewprojection", shadow_proj);

	if (texture)
		shader->setUniform("u_texture", texture, 0);

	shader->setUniform("u_model", model);
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);
	
	mesh->render(GL_TRIANGLES);

	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS); //as default
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterial(const Matrix44& model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	Texture* texture = NULL;
	Texture* texture_em = NULL;
	Texture* texture_met_rough = NULL;
	Texture* texture_norm = NULL;

	texture = material->color_texture.texture;
	texture_em = material->emissive_texture.texture;
	texture_met_rough = material->metallic_roughness_texture.texture;
	texture_norm = material->normal_texture.texture;
	//texture = material->occlusion_texture;

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	switch (render_mode) {
		case FORWARD:
			if (light_mode == MULTI) { shader = Shader::Get("light_multi"); }
			if (light_mode == SINGLE) { shader = Shader::Get("light_single"); }
			break;
		case SHOW_TEXTURE:
			shader = Shader::Get("texture");
			break;
		case SHOW_UVS:
			shader = Shader::Get("uvs");
			break;
		case SHOW_NORMALS:
			shader = Shader::Get("normals");
			break;
		case SHOW_OCCLUSION:
			shader = Shader::Get("occlusion");
			break;
		case SHOW_EMISSIVE:
			shader = Shader::Get("emissive");
			break;
		case DEFERRED:
			shader = Shader::Get("gbuffers");
			break;
	}

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_viewmatrix", camera->view_matrix);
	shader->setUniform("u_model", model);
	shader->setUniform("u_color", material->color);
	shader->setUniform("u_emissive", material->emissive_factor);
	shader->setVector3("u_ambient_light", GTR::Scene::instance->ambient_light);
	shader->setUniform("u_light_eq", light_eq);
	shader->setUniform("u_gamma", hdr_gamma);

	if (!texture)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture
	if (!texture_met_rough)
		texture_met_rough = Texture::getWhiteTexture(); //a 1x1 white texture

	shader->setUniform("u_metallic", material->metallic_factor);
	shader->setUniform("u_roughness", material->roughness_factor);

	if (!texture_em)
		texture_em = Texture::getWhiteTexture(); //a 1x1 white texture
	if (!texture_norm)
		texture_norm = Texture::getBlackTexture(); //a 1x1 white texture

	shader->setUniform("u_texture", texture, 0);
	shader->setUniform("u_texture_em", texture_em, 1);
	shader->setUniform("u_texture_metallic_roughness", texture_met_rough, 2);
	shader->setUniform("u_texture_normals", texture_norm, 3);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);


	if (render_mode == FORWARD && light_mode == MULTI)
	{
		//select the blending
		if (material->alpha_mode == GTR::eAlphaMode::BLEND)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		else
		{
			glDisable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		}

		if (lights.size() == 0) //Taking care of the "no lights" scenario
		{
			shader->setUniform("u_light_type", (int)NO_LIGHT);
			mesh->render(GL_TRIANGLES);
		}
		else
			renderMultiPass(mesh, material, shader);
	}
	else if (render_mode == FORWARD && light_mode == SINGLE)
	{
		//select the blending
		if (material->alpha_mode == GTR::eAlphaMode::BLEND)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}

		renderSinglePass(shader, mesh);
	}
	else
		if (dithering || material->alpha_mode != BLEND)
			mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS); //as default
}

void Renderer::renderMultiPass(Mesh* mesh, Material* material, Shader* shader)
{
	//allow to render pixels that have the same depth as the one in the depth buffer
	glDepthFunc(GL_LEQUAL);

	for (int i = 0; i < lights.size(); ++i)
	{
		LightEntity* light = lights[i];

		//first pass doesn't use blending
		if (i != 0)
		{
			if (material) {
				if (material->alpha_mode != GTR::eAlphaMode::BLEND) { glEnable(GL_BLEND); }
				if (material->alpha_mode == GTR::eAlphaMode::BLEND) { glBlendFunc(GL_SRC_ALPHA, GL_ONE); }
			}
			shader->setVector3("u_ambient_light", Vector3(0, 0, 0));
			if (render_mode == DEFERRED) { shader->setUniform("u_emissive", false); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
			else { shader->setUniform("u_emissive", Vector3(0, 0, 0)); }
		}

		light->uploadLightParams(shader, true, hdr_gamma);

		//render the mesh
		mesh->render(GL_TRIANGLES);
	}
}

void Renderer::renderMultiPassSphere(Shader* sh, Camera* camera)
{
	//first pass doesn't use blending)
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glEnable(GL_CULL_FACE);

	sh->setVector3("u_ambient_light", Vector3(0, 0, 0));
	sh->setUniform("u_emissive", false);
	sh->setUniform("u_back", false);
	sh->setUniform("u_light_eq", light_eq);
	sh->setUniform("u_camera_position", camera->eye);

	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", false);

	for (int i = 0; i < lights.size(); ++i)
	{
		LightEntity* light = lights[i];

		//basic.vs will need the model and the viewproj of the camera
		sh->setUniform("u_viewprojection", camera->viewprojection_matrix);

		//we must translate the model to the center of the light
		Matrix44 m;
		Vector3 pos = light->model.getTranslation();
		m.setTranslation(pos.x, pos.y, pos.z);
		//and scale it according to the max_distance of the light
		m.scale(light->max_distance, light->max_distance, light->max_distance);

		//pass the model to the shader to render the sphere
		sh->setUniform("u_model", m);

		light->uploadLightParams(sh, true, hdr_gamma);

		glFrontFace(GL_CW);

		//render the mesh
		sphere->render(GL_TRIANGLES);
	}

	//disable depth test and blend!!
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glFrontFace(GL_CCW);
}

void Renderer::renderSinglePass(Shader* shader, Mesh* mesh)
{
	//Defining the vectors that will be passed to the GPU
	Matrix44 shadow_proj[max_lights];
	Vector3 light_position[max_lights];
	Vector3 light_color[max_lights];
	Vector3 light_direction[max_lights];
	Vector3 light_uvs[max_lights];
	int light_type[max_lights];
	float light_maxdistance[max_lights];
	float light_intensity[max_lights];
	float light_cutoff[max_lights];
	float light_exponent[max_lights];
	float light_bias[max_lights];
	int light_shadows[max_lights];

	//Filling the vectors
	for (int i = 0; i < lights.size(); ++i)
	{
		LightEntity* light = lights[i];

		light_position[i] = light->model * Vector3(0, 0, 0);

		Vector3 l_color = Vector3(pow(light->color.x, hdr_gamma), pow(light->color.y, hdr_gamma), pow(light->color.z, hdr_gamma));
		light_color[i] = l_color;

		light_maxdistance[i] = light->max_distance;
		light_type[i] = (int)light->light_type;
		light_intensity[i] = light->intensity;
		light_cutoff[i] = cos(light->cone_angle * PI / 180);;
		light_direction[i] = light->model.frontVector();
		light_exponent[i] = light->spot_exp;
		light_shadows[i] = (int)light->cast_shadows;
		light_bias[i] = light->bias;
		shadow_proj[i] = light->camera->viewprojection_matrix;
		light_uvs[i] = light->uvs;
	}

	//Passing all the vectors to the GPU
	shader->setMatrix44Array("u_shadow_viewproj", shadow_proj, max_lights);
	shader->setUniform3Array("u_light_position", (float*)&light_position, max_lights);
	shader->setUniform3Array("u_light_color", (float*)&light_color, max_lights);
	shader->setUniform3Array("u_light_vector", (float*)&light_direction, max_lights);
	shader->setUniform3Array("u_light_uvs", (float*)&light_uvs, max_lights);
	shader->setUniform1Array("u_light_maxdist", (float*)&light_maxdistance, max_lights);
	shader->setUniform1Array("u_light_type", (int*)&light_type, max_lights);
	shader->setUniform1Array("u_light_intensity", (float*)&light_intensity, max_lights);
	shader->setUniform1Array("u_light_cutoff", (float*)&light_cutoff, max_lights);
	shader->setUniform1Array("u_light_exp", (float*)&light_exponent, max_lights);
	shader->setUniform1Array("u_shadows", (int*)&light_shadows, max_lights);
	shader->setUniform1Array("u_shadow_bias", (float*)&light_bias,max_lights);
	shader->setUniform1("u_num_lights", (int)lights.size());
	shader->setUniform("u_shadow_count", shadow_count);
	shader->setUniform("u_pcf", pcf);
	shader->setUniform("u_texture_atlas", atlas->depth_texture, 8);

	//render the mesh
	mesh->render(GL_TRIANGLES);
}

void Renderer::renderToAtlas(Camera* camera) {

	//if render mode is not singlepass or there are no lights or prefabs to show, return
	if (shadow_count == 0 || calls.empty())
		return;

	Shader* shader = NULL;
	shader = Shader::Get("shadowmap");
	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	int res = 1024 * pow(2, (int)Application::instance->quality); // resolution of texture (1:1 aspect ratio)
	//first time we create the FBO
	if (atlas == NULL)
	{
		atlas = new FBO();
		atlas->setDepthOnly(res * (int)ceil(sqrt(shadow_count)), res * (int)ceil(sqrt(shadow_count))); //will always be squared
	}

	atlas->bind();

	//you can disable writing to the color buffer to speed up the rendering as we do not need it
	glColorMask(false, false, false, false);

	//need to enable scissor test to clear buffer of current window
	glEnable(GL_SCISSOR_TEST);

	int c = 0; // current light
	//traverse lights
	for (int i = 0; i < lights.size(); ++i) {
		
		LightEntity* light = lights[i];

		if (light->light_type == POINT)
			continue;

		updateLight(light, camera);

		//clear the depth buffer only (don't care of color) and set viewport to current light
		float len = ceil(sqrt(shadow_count));
		int ires = (c % (int)len) * res; //x iterators on texture
		int jres = (int)(floor(c / len)) * res; //y iterators on texture
		//we may want to pass to shader
		float wi = (c % (int)len) / len;
		float hj = floor(c / len) / len;
		
		light->uvs = Vector3(wi, hj, 1/len);
		glScissor(ires, jres, res, res);
		glClear(GL_DEPTH_BUFFER_BIT);
		glViewport(ires, jres, res, res);
		//traverse all prefabs that dont use blending
		for (int i = 0; i < calls.size(); ++i) {
			//if prefab is inside the light's camera frustum render it
			BoundingBox aabb = transformBoundingBox(calls[i].model, calls[i].mesh->box);
			if ((!light->camera->testBoxInFrustum(aabb.center, aabb.halfsize) && light->light_type != DIRECTIONAL) || calls[i].material->alpha_mode == BLEND) {
				continue;
			}
			//select if render both sides of the triangles
			if (calls[i].material->two_sided)
				glDisable(GL_CULL_FACE);
			else
				glEnable(GL_CULL_FACE);
			assert(glGetError() == GL_NO_ERROR);
			Mesh* mesh = calls[i].mesh;
			Matrix44 model = calls[i].model;
			Texture* c_texture = calls[i].material->color_texture.texture;
			shader->setUniform("u_viewprojection", light->camera->viewprojection_matrix);
			shader->setUniform("u_model", model);
			shader->setUniform("u_texture", c_texture, 5);
			shader->setUniform("u_alpha_cutoff", calls[i].material->alpha_mode == GTR::eAlphaMode::MASK ? calls[i].material->alpha_cutoff : 0);
			mesh->render(GL_TRIANGLES);
		}
		c++; //update light counter
	}
	//unbind to render back to the screen
	atlas->unbind();
	shader->disable();
	int w = Application::instance->window_width;
	int h = Application::instance->window_height;
	glViewport(0, 0, w, h);

	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_CULL_FACE);

	//allow to render back to the color buffer
	glColorMask(true, true, true, true);

}

void Renderer::renderAtlas() {

	Shader* atlas_shader = Shader::Get("atlas");
	glDisable(GL_BLEND);

	int w = Application::instance->window_width;
	int h = Application::instance->window_height;

	int sz = shadow_count;
	int c = 0; //current light
	Vector2 nearfars[max_lights];
	int light_types[max_lights];
	//from vector of lights use only those that cast shadows
	for (int i = 0; i < lights.size(); ++i) {
		LightEntity* l = lights[i];
		if (l->light_type == POINT || !l->cast_shadows)
			continue;
		//add light params to vectors
		nearfars[c] = Vector2(l->camera->near_plane, l->camera->far_plane);
		light_types[c] = l->light_type;
		c++;
	}
	atlas_shader->enable();

	//pass entire vectors to the shader
	atlas_shader->setUniform2Array("u_camera_nearfars", (float*)&nearfars, c);
	atlas_shader->setUniform1Array("u_light_types", (int*)&light_types, c); //POINT = 0, SPOT = 1, DIRECTIONAL = 2
	atlas_shader->setUniform("u_total_lights", c);

	//send to viewport
	int offset = 30;
	glViewport(offset, offset, w * 0.5 - offset, h * 0.5 - offset);
	atlas->depth_texture->toViewport(atlas_shader);

	atlas_shader->disable();
	//set viewport back to default
	glViewport(0, 0, w, h);
}

void Renderer::showGbuffers(FBO* gbuffers_fbo, Camera* camera)
{
	//gbuffers_fbo->color_textures[0]->toViewport();
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;

	if (show_omr)
	{
		Shader* omr_shader = Shader::Get("omr");

		glViewport(0, 0, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[2]->toViewport(omr_shader); //metallic bottom left

		glViewport(width * 0.5, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[1]->toViewport(omr_shader); //occlusion texture top right

		glViewport(0, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[0]->toViewport(omr_shader); //roughness top left

		glViewport(width * 0.5, 0, width * 0.5, height * 0.5);
		if (activate_ssao)
		{
			if (ssao->blur)
				ssao->ssao_fbo->color_textures[1]->toViewport(); //SSAO bottom right
			else
				ssao->ssao_fbo->color_textures[0]->toViewport(); //SSAO bottom right
		}
	}
	else 
	{
		Shader* hdr_shader = Shader::Get("hdr");

		hdr_shader->enable();
		hdr_shader->setUniform("u_hdr", false);

		glViewport(0, 0, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[0]->toViewport(hdr_shader);

		glViewport(width * 0.5, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[2]->toViewport(hdr_shader);

		glViewport(width * 0.5, 0, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[1]->toViewport();

		glViewport(0, height * 0.5, width * 0.5, height * 0.5);
		Shader* depth_shader = Shader::Get("depth");
		depth_shader->enable();
		depth_shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
		gbuffers_fbo->depth_texture->toViewport(depth_shader);
	}
}


Texture* GTR::CubemapFromHDRE(const char* filename)
{
	HDRE* hdre = new HDRE();
	if (!hdre->load(filename))
	{
		delete hdre;
		return NULL;
	}

	return NULL;
}

SSAO::SSAO(int points_num, bool ssao_plus)
{
	intensity = 1.0;
	samples = points_num;
	plus = ssao_plus;
	bias = 0.005;
	blur = true;

	Texture* ssao_texture = new Texture(Application::instance->window_width * 0.5, Application::instance->window_height * 0.5, GL_LUMINANCE, GL_UNSIGNED_BYTE);
	Texture* ssao_texture_blur = new Texture(Application::instance->window_width * 0.5, Application::instance->window_height * 0.5, GL_LUMINANCE, GL_UNSIGNED_BYTE);
	std::vector<Texture*> textures = { ssao_texture, ssao_texture_blur };

	ssao_fbo = new FBO();
	ssao_fbo->setTextures(textures);

	points = generateSpherePoints(samples, 1.0f, plus);
}

Texture* SSAO::apply(Texture* normal_buffer, Texture* depth_buffer, Camera* camera)
{
	if (!depth_buffer->filtered)
	{
		//bind the texture we want to change
		depth_buffer->bind();

		//disable using mipmaps
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

		//enable bilinear filtering
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		depth_buffer->unbind();

		depth_buffer->filtered = true;
	}

	Mesh* quad = Mesh::getQuad();

	//start rendering inside the ssao texture
	ssao_fbo->bind();

	//get the shader for SSAO (remember to create it using the atlas)
	Shader* shader = Shader::Get("ssao");
	shader->enable();

	//send info to reconstruct the world position
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_normal_texture", normal_buffer, 1);
	shader->setTexture("u_depth_texture", depth_buffer, 4);

	//we need the pixel size so we can center the samples 
	shader->setUniform("u_iRes", Vector2(1.0 / (float)depth_buffer->width, 1.0 / (float)depth_buffer->height));

	//we will need the viewprojection to obtain the uv in the depthtexture of any random position of our world
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);

	//send random points so we can fetch around
	shader->setUniform3Array("u_points", (float*)&points[0],points.size());
	shader->setUniform("u_samples", samples);
	shader->setUniform("u_ssao_plus", plus);
	shader->setUniform("u_bias", bias);

	//render fullscreen quad
	quad->render(GL_TRIANGLES);

	//stop rendering to the texture
	ssao_fbo->unbind();

	ssao_fbo->bind();

	if (blur)
	{
		//get the shader for SSAO (remember to create it using the atlas)
		shader = Shader::Get("ssao_blur");
		shader->enable();

		shader->setUniform("u_ssao", ssao_fbo->color_textures[0], 0);

		quad->render(GL_TRIANGLES);

		shader->disable();
	}

	ssao_fbo->unbind();

	if (blur)
		return ssao_fbo->color_textures[1];
	else
		return ssao_fbo->color_textures[0];
}

void Renderer::passDeferredUniforms(Shader* sh, bool first_pass, Camera* camera, int& w, int& h)
{
	//pass the gbuffers to the shader
	sh->setUniform("u_color_texture", gbuffers_fbo->color_textures[0], 0);
	sh->setUniform("u_normal_texture", gbuffers_fbo->color_textures[1], 1);
	sh->setUniform("u_extra_texture", gbuffers_fbo->color_textures[2], 2);
	sh->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 4);

	//pass the inverse projection of the camera to reconstruct world pos.
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();
	sh->setUniform("u_inverse_viewprojection", inv_vp);
	//pass the inverse window resolution, this may be useful
	sh->setUniform("u_iRes", Vector2(1.0 / (float)w, 1.0 / (float)h));

	//Light uniforms
	if (first_pass)
	{
		sh->setVector3("u_ambient_light", GTR::Scene::instance->ambient_light);
		sh->setUniform("u_emissive", true);
		sh->setUniform("u_back", true);
		sh->setUniform("u_ao", activate_ssao);
	}
	else {
		sh->setVector3("u_ambient_light", Vector3(0, 0, 0));
		sh->setUniform("u_emissive", false);
		sh->setUniform("u_back", false);
		sh->setUniform("u_ao", false);
	}

	sh->setUniform("u_light_eq", light_eq);
	sh->setUniform("u_camera_position", camera->eye);
	sh->setUniform("u_gamma", hdr_gamma);
}