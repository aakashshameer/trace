﻿/****************************************************************************
 * Copyright ©2017 Brian Curless.  All rights reserved.  Permission is hereby
 * granted to students registered for University of Washington CSE 457 or CSE
 * 557 for use solely during Autumn Quarter 2017 for purposes of the course.
 * No other use, copying, distribution, or modification is permitted without
 * prior written consent. Copyrights for third-party components of this work
 * must be honored.  Instructors interested in reusing these course materials
 * should contact the author.
 ****************************************************************************/
#include "trace/raytracer.h"
#include <scene/scene.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <glm/gtx/component_wise.hpp>
#include <scene/components/triangleface.h>
#include <glm/gtx/string_cast.hpp>
#include "components.h"

unsigned int pow4(unsigned int e) {
    unsigned int i = 1;
    while(e>0) {
        i *= 4;
        e--;
    }
    return i;
}

RayTracer::RayTracer(Scene& scene, SceneObject& camobj) :
    trace_scene(&scene, camobj.GetComponent<Camera>()->TraceEnableAcceleration.Get()), next_render_index(0), cancelling(false), first_pass_buffer(nullptr)
{
    Camera* cam = camobj.GetComponent<Camera>();

    settings.width = cam->RenderWidth.Get();
    settings.height = cam->RenderHeight.Get();
    settings.pixel_size_x = 1.0/double(settings.width);
    settings.pixel_size_y = 1.0/double(settings.height);

    settings.shadows = cam->TraceShadows.Get() != Camera::TRACESHADOWS_NONE;
    settings.translucent_shadows = cam->TraceShadows.Get() == Camera::TRACESHADOWS_COLORED;
    settings.reflections = cam->TraceEnableReflection.Get();
    settings.refractions = cam->TraceEnableRefraction.Get();
    settings.env_mapping = cam->TraceEnableEnvMapping.Get();
    settings.env_map = cam->TraceEnvMap.Get();
    settings.fresnel = cam->TraceFresnel.Get();
    settings.beers = cam->TraceBeers.Get();

    settings.random_mode = cam->TraceRandomMode.Get();
    settings.diffuse_reflection = cam->TraceEnableReflection.Get() && cam->TraceDiffuseReflection.Get() && settings.random_mode != Camera::TRACERANDOM_DETERMINISTIC;
    settings.caustics = settings.diffuse_reflection && settings.shadows && cam->TraceCaustics.Get();
    settings.random_branching = cam->TraceRandomBranching.Get() && settings.random_mode != Camera::TRACERANDOM_DETERMINISTIC;

    settings.samplecount_mode = cam->TraceSampleCountMode.Get();

    settings.constant_samples_per_pixel = pow4(cam->TraceConstantSampleCount.Get());
    settings.dynamic_sampling_min_depth = cam->TraceSampleMinCount.Get();
    settings.dynamic_sampling_max_depth = cam->TraceSampleMaxCount.Get()+1;
    settings.adaptive_max_diff_squared = cam->TraceAdaptiveSamplingMaxDiff.Get();
    settings.adaptive_max_diff_squared *= settings.adaptive_max_diff_squared;
    settings.max_stderr = cam->TraceStdErrorSamplingCutoff.Get();

    if (settings.samplecount_mode == Camera::TRACESAMPLING_RECURSIVE && settings.random_mode!=Camera::TRACERANDOM_DETERMINISTIC) {
        qDebug() << "Adaptive Recursive Supersampling does not work with Monte Carlo!";
        settings.samplecount_mode = Camera::TRACESAMPLING_CONSTANT;
    }

    settings.max_depth = cam->TraceMaxDepth.Get();

    //camera looks -z, x is right, y is up
    glm::mat4 camera_matrix = camobj.GetModelMatrix();

    settings.projection_origin = glm::vec3(camera_matrix * glm::vec4(0,0,0,1));

    glm::dvec3 fw_point = glm::dvec3(camera_matrix * glm::vec4(0,0,-1,1));
    glm::dvec3 x_point = glm::dvec3(camera_matrix * glm::vec4(1,0,0,1));
    glm::dvec3 y_point = glm::dvec3(camera_matrix * glm::vec4(0,1,0,1));
    glm::dvec3 fw_vec = glm::normalize(fw_point - settings.projection_origin);
    glm::dvec3 x_vec = glm::normalize(x_point - settings.projection_origin);
    glm::dvec3 y_vec = glm::normalize(y_point - settings.projection_origin);

    //FOV is full vertical FOV
    double tangent = tan( (cam->FOV.Get()/2.0) * 3.14159/180.0 );

    double focus_dist = cam->TraceFocusDistance.Get();

    settings.projection_forward = focus_dist * fw_vec;
    settings.projection_up = focus_dist * tangent * y_vec;
    settings.projection_right = focus_dist * tangent * AspectRatio() * x_vec;

    double aperture_radius = cam->TraceApertureSize.Get() * 0.5;
    settings.aperture_up = aperture_radius * y_vec;
    settings.aperture_right = aperture_radius * x_vec;
    settings.aperture_radius = aperture_radius;

    buffer = new uint8_t[settings.width * settings.height * 3]();

    int num_threads = QThread::idealThreadCount();
    if (num_threads > 1) {
        num_threads -= 1; //leave a free thread so the computer doesn't totally die
    }
    thread_pool.setMaxThreadCount(num_threads);

    if (settings.samplecount_mode!=Camera::TRACESAMPLING_CONSTANT && settings.dynamic_sampling_min_depth==0) {
        int orig = settings.samplecount_mode;
        settings.samplecount_mode = Camera::TRACESAMPLING_CONSTANT;
        settings.constant_samples_per_pixel = 1;

        //TODO make it not hang here
        for (unsigned int i = 0; i < num_threads; i++) {
            thread_pool.start(new RTWorker(*this));
        }
        thread_pool.waitForDone(-1);
        next_render_index.store(0);

        settings.samplecount_mode = orig;
        first_pass_buffer = buffer;
        buffer = new uint8_t[settings.width * settings.height * 3]();
    }

    // Spin off threads
    for (unsigned int i = 0; i < num_threads; i++) {
        thread_pool.start(new RTWorker(*this));
    }
}

RayTracer::~RayTracer() {
    cancelling=true;
    thread_pool.waitForDone(-1);
    delete[] buffer;
    if (first_pass_buffer != nullptr) {
        delete[] first_pass_buffer;
    }
    if (debug_camera_used_) {
        debug_camera_used_->ClearDebugRays();
    }
}

int RayTracer::GetProgress() {
    if (thread_pool.waitForDone(1)) {
        return 100;
    }

    const unsigned int wc = (settings.width+THREAD_CHUNKSIZE-1)/THREAD_CHUNKSIZE;
    const unsigned int hc = (settings.height+THREAD_CHUNKSIZE-1)/THREAD_CHUNKSIZE;
    int complete = (100*next_render_index.fetchAndAddRelaxed(0))/(wc*hc);
    return std::min(complete,99);
}

double RayTracer::AspectRatio() {
    return ((double)settings.width)/((double)settings.height);
}


void RayTracer::ComputePixel(int i, int j, Camera* debug_camera) {
    // Calculate the normalized coordinates [0, 1]
    double x_corner = i * settings.pixel_size_x;
    double y_corner = j * settings.pixel_size_y;

    if (debug_camera) {
        if (debug_camera_used_) {
            debug_camera_used_->ClearDebugRays();
        }
        debug_camera_used_ = debug_camera;
        SampleCamera(x_corner, y_corner, settings.pixel_size_x, settings.pixel_size_y, debug_camera);
        return;
    }

    // Trace the ray!
    glm::vec3 color(0,0,0);
    int n = sqrt(settings.constant_samples_per_pixel);

    switch (settings.samplecount_mode) {
        case Camera::TRACESAMPLING_CONSTANT:
            // REQUIREMENT: Implement Anti-aliasing
            //              use setting.constant_samples_per_pixel to get the amount of samples of a pixel for anti-alasing
            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    color += SampleCamera(x_corner + i*settings.pixel_size_x/n, y_corner + j*settings.pixel_size_y/n, settings.pixel_size_x/n, settings.pixel_size_y/n, debug_camera);
                }
            }
            color = color/(1.0f*n*n);
            break;
        default:
            break;
    }

    color = glm::clamp(color, 0.0f, 1.0f);

    // Set the pixel in the render buffer
    uint8_t* pixel = buffer + (i + j * settings.width) * 3;
    pixel[0] = (uint8_t)( 255.0f * color[0]);
    pixel[1] = (uint8_t)( 255.0f * color[1]);
    pixel[2] = (uint8_t)( 255.0f * color[2]);
}


glm::vec3 RayTracer::SampleCamera(double x_corner, double y_corner, double pixel_size_x, double pixel_size_y, Camera* debug_camera)
{
    double x = x_corner + pixel_size_x * 0.5;
    double y = y_corner + pixel_size_y * 0.5;

    glm::dvec3 point_on_focus_plane = settings.projection_origin + settings.projection_forward + (2.0*x-1.0)*settings.projection_right + (2.0*y-1.0)*settings.projection_up;

    glm::vec2 sample = glm::dvec2(0,0);
    double angle = sample.x;
    double radius = sqrt(sample.y);

    glm::dvec3 origin = settings.projection_origin + radius * (sin(angle) * settings.aperture_up + cos(angle) * settings.aperture_right);

    glm::dvec3 dir = glm::normalize(point_on_focus_plane - origin);

    Ray camera_ray(origin, dir);

    return TraceRay(camera_ray, 0, RayType::camera, debug_camera);
}

// Do recursive ray tracing!  You'll want to insert a lot of code here
// (or places called from here) to handle reflection, refraction, etc etc.
// Depth is the number of times the ray has intersected an object.
glm::vec3 RayTracer::TraceRay(const Ray& r, int depth, RayType ray_type, Camera* debug_camera)
{
    Intersection i;

    if (debug_camera) {
        glm::dvec3 endpoint = r.at(1000);
        if (trace_scene.Intersect(r, i)) {
            endpoint = r.at(i.t);
            debug_camera->AddDebugRay(endpoint, endpoint+0.25*(glm::dvec3)i.normal, RayType::hit_normal);
        }
        debug_camera->AddDebugRay(r.position, endpoint, ray_type);
    }

    if (trace_scene.Intersect(r, i)) {     
        // REQUIREMENT: Implement Raytracing
        // You must implement (see project page for details)
        // 1. Blinn-Phong specular model
        // 2. Light contribution
        // 3. Shadow attenuation
        // 4. Reflection
        // 5. Refraction
        // 6. Anti-Aliasing

        // An intersection occured!  We've got work to do. For now,
        // this code gets the material parameters for the surface
        // that was intersected.
        Material* mat = i.GetMaterial();
        glm::vec3 kd = mat->Diffuse->GetColorUV(i.uv);
        glm::vec3 ks = mat->Specular->GetColorUV(i.uv);
        glm::vec3 ke = mat->Emissive->GetColorUV(i.uv);
        glm::vec3 kt = mat->Transmittence->GetColorUV(i.uv);
        float shininess = mat->Shininess;
        double index_of_refraction = mat->IndexOfRefraction;

        // Interpolated normal
        // Use this to when calculating geometry (entering object test, reflection, refraction, etc) or getting smooth shading (light direction test, etc)
        glm::vec3 N = i.normal;

        // return kd;

        // This is a great place to insert code for recursive ray tracing.
        // Compute the blinn-phong shading, and don't stop there:
        // add in contributions from reflected and refracted rays.

        glm::vec3 sum = ke;

        // To iterate over all light sources in the scene, use code like this:
         for (auto j = trace_scene.lights.begin(); j != trace_scene.lights.end(); j++) {
           TraceLight* trace_light = *j;
           //Light* scene_light = trace_light->light;

           sum += ComputeBlinnPhong(r, i.t, kd, ks, ke, kt, shininess, N, trace_light, debug_camera);
         }
         //std::cout << sum.x << sum.y << sum.z << std::endl;
         if (depth == settings.max_depth) {
             return sum;
         }
        // Make sure to test if the Reflections and Refractions checkboxes are enabled in the Render Cam UI
        // Use this condition, only calculate reflection/refraction if enabled:
         glm::vec3 zero = {0, 0, 0};
         glm::vec3 V = -r.direction;
         if (settings.reflections && ks != zero) {
             glm::vec3 norm = dot(N, -V) > 0 ? -N : N;
             glm::vec3 dir = 2.0f*dot(V, norm)*norm - V;
             Ray ray(r.at(i.t), dir);
             sum += ks * TraceRay(ray, depth + 1, RayType::reflection, debug_camera);
         }
         if (settings.refractions && kt != zero) {
             float eta;
             float cos_inc;
             glm::vec3 norm;
             if (dot(N, -V) <= 0) {
                 eta = INDEX_OF_AIR/index_of_refraction;
                 norm = N;
             } else {
                 eta = index_of_refraction/INDEX_OF_AIR;
                 norm = -N;
             }
             cos_inc = dot(norm, V);
             float disc = 1.0 - eta*eta*(1.0 - cos_inc*cos_inc);
             if (disc >= 0) {
                 float cos_trans = sqrt(disc);
                 glm::vec3 dir = (eta*cos_inc - cos_trans)*norm - eta*V;
                 Ray ray(r.at(i.t), dir);
                 sum += kt * TraceRay(ray, depth + 1, RayType::refraction, debug_camera);
             }
         }
         return sum;
    } else {
        // No intersection. This ray travels to infinity, so we color it according to the background color,
        // which in this (simple) case is just black.
        // EXTRA CREDIT: Use environment mapping to determine the color instead
        glm::vec3 background_color = glm::vec3(0, 0, 0);
        if (!settings.env_mapping) {
            return background_color;
        }

        enum CubeSide{
            FRONT = 0,
            BACK = 1,
            TOP = 2,
            BOTTOM = 3,
            RIGHT = 4,
            LEFT = 5
        };
        double max_comp = fmax(abs(r.direction.x), abs(r.direction.y));
        max_comp = fmax(max_comp, abs(r.direction.z));
        glm::dvec2 uv;
        CubeSide s;
        if (max_comp == abs(r.direction.x)) {
            if (r.direction.x < 0) {
                s = CubeSide::LEFT;
                uv = {r.direction.z, -r.direction.y};
            } else {
                s = CubeSide::RIGHT;
                uv = {-r.direction.z, -r.direction.y};
            }
        } else if (max_comp == abs(r.direction.y)) {
            if (r.direction.y < 0) {
                s = CubeSide::BOTTOM;
                uv = {r.direction.x, -r.direction.z};
            } else {
                s = CubeSide::TOP;
                uv = {r.direction.x, r.direction.z};
            }
        } else {
            if (r.direction.z < 0) {
                s = CubeSide::BACK;
                uv = {-r.direction.x, -r.direction.y};
            } else {
                s = CubeSide::FRONT;
                uv = {r.direction.x, -r.direction.y};
            }
        }
        uv = (uv + 1.0)/2.0;
        const unsigned char* face = settings.env_map->GetFace(s);
        unsigned int res = settings.env_map->GetResolution();
        unsigned int u = (uv.x*res);
        unsigned int v = (uv.y*res);
        unsigned int idx = v*res*4 + u*4;
        background_color.x = face[idx]/255.0f;
        background_color.y = face[idx+1]/255.0f;
        background_color.z = face[idx+2]/255.0f;

        return background_color;
    }
}

glm::vec3 RayTracer::ComputeBlinnPhong (const Ray& r, double t, glm::vec3 kd, glm::vec3 ks, glm::vec3 ke, glm::vec3 kt, float shininess, glm::vec3 N, TraceLight* tl, Camera* debug_camera) {

    Light* scene_light = tl->light;
    glm::vec3 V = -r.direction;
    glm::vec3 L;
    glm::vec3 I;
    glm::vec3 ambient;
    float A_dist = 1.0;


    if(PointLight* pl = dynamic_cast<PointLight*>(scene_light)) {
        // point light
        glm::dvec3 pos = tl->GetTransformPos();
        L = normalize(pos - r.at(t));

        if (AttenuatingLight* al = dynamic_cast<AttenuatingLight*>(scene_light)) {
            double a = al->AttenA.Get();
            double b = al->AttenB.Get();
            double c = al->AttenC.Get();

            double dist = a * length(pos - r.at(t)) * length(pos - r.at(t)) + b * length(pos - r.at(t)) + c;

            A_dist = fmin(1.0, 1.0/dist);
        }
        I = pl->GetIntensity();
        ambient = kd * pl->Ambient.GetRGB();

    } else if (DirectionalLight* dl = dynamic_cast<DirectionalLight*>(scene_light)) {
        // directional light

        L = tl->GetTransformDirection();
        I = dl->GetIntensity();
        ambient = kd * dl->Ambient.GetRGB();
    }
    glm::vec3 norm = dot(N, -V) > 0 ? -N : N;

    float B = 1.0;
    if (dot(norm, L) < 0.00001) {B = 0.0;}

    Ray* ray = new Ray(r.at(t), L);
    glm::vec3 A_shad = ShadowAttenuation(ray, tl, debug_camera);
    //glm::vec3 A_shad = {1, 1, 1};
    glm::vec3 H = normalize(V + L);
    float shiny = shininess > 0 ? shininess : 0.00001;
    glm::vec3 diffuse = kd * fmax(dot(norm, L), 0.0f);
    glm::vec3 specular = ks * pow(fmax(dot(norm,H), 0.0f),shiny);
    ambient = dot(norm, -V) > 0 ? kt * ambient : ambient;

    glm::vec3 result = ambient + (A_shad * A_dist * I * B * (diffuse + specular));
    delete(ray); // deallocate ray
    return result;
}

glm::vec3 RayTracer::ShadowAttenuation (Ray* r, TraceLight* tl, Camera* debug_camera) {
    Intersection i;
    Light* scene_light = tl->light;
//    if (!trace_scene.Intersect(*r, i)) {
//        return {1,1,1};
//    } else {

//        Material* m = i.GetMaterial();
//        glm::vec3 kt = m->Transmittence->GetColorUV(i.uv);
//        Ray ray(r->at(i.t) + RAY_EPSILON, r->direction);
//        return kt * ShadowAttenuation(&ray);
//    }
//    if (trace_scene.Intersect(*r, i)) {
//        r->position = r->at(i.t);
//    }
    if (!trace_scene.Intersect(*r, i)) {
        if (PointLight* pl = dynamic_cast<PointLight*>(scene_light)) {
            glm::dvec3 pos = tl->GetTransformPos();
            if (debug_camera) {
                debug_camera->AddDebugRay(r->position, pos, RayType::shadow);
            }
        } else {
            if (debug_camera) {
                debug_camera->AddDebugRay(r->position, r->at(1000), RayType::shadow);
            }
        }
    }

    glm::vec3 A_shad = {1, 1, 1};
    while (trace_scene.Intersect(*r, i)) {
        if (PointLight* pl = dynamic_cast<PointLight*>(scene_light)) {
            glm::dvec3 pos = tl->GetTransformPos();
            if (length(r->at(i.t) - r->position) > length(pos - r->position)) {
                break;
            }
        }
        if (debug_camera) {
            debug_camera->AddDebugRay(r->position, r->at(i.t), RayType::shadow);
        }
        Material* m = i.GetMaterial();
        glm::vec3 kt = m->Transmittence->GetColorUV(i.uv);
        A_shad *= kt;
        r->position = r->at(i.t);
    }
    return A_shad;
}


// Multi-Threading
RTWorker::RTWorker(RayTracer &tracer_) :
    tracer(tracer_) { }

void RTWorker::run() {
    // Dimensions, in chunks
    const unsigned int wc = (tracer.settings.width+THREAD_CHUNKSIZE-1)/THREAD_CHUNKSIZE;
    const unsigned int hc = (tracer.settings.height+THREAD_CHUNKSIZE-1)/THREAD_CHUNKSIZE;

    unsigned int x, y;
    while (!tracer.cancelling) {
        unsigned int idx = tracer.next_render_index.fetchAndAddRelaxed(1);
        unsigned int x = (idx%wc)*THREAD_CHUNKSIZE;
        unsigned int y = (idx/wc)*THREAD_CHUNKSIZE;
        if (y >= tracer.settings.height) break;
        unsigned int maxX = std::min(x + THREAD_CHUNKSIZE, tracer.settings.width);
        unsigned int maxY = std::min(y + THREAD_CHUNKSIZE, tracer.settings.height);

        for(unsigned int yy = y; yy < maxY && !tracer.cancelling; yy++) {
            for(unsigned int xx = x; xx < maxX && !tracer.cancelling; xx++) {
                tracer.ComputePixel(xx, yy);
            }
        }
    }
}
