/****************************************************************************
 * Copyright ©2017 Brian Curless.  All rights reserved.  Permission is hereby
 * granted to students registered for University of Washington CSE 457 or CSE
 * 557 for use solely during Autumn Quarter 2017 for purposes of the course.
 * No other use, copying, distribution, or modification is permitted without
 * prior written consent. Copyrights for third-party components of this work
 * must be honored.  Instructors interested in reusing these course materials
 * should contact the author.
 ****************************************************************************/
#ifndef PARTICLESYSTEM_H
#define PARTICLESYSTEM_H

#include <signal.h>
#include <scene/components/component.h>
#include <resource/material.h>

struct Particle {
    // Modify this struct as you see fit.
    // Or don't use it at all if you want.
    glm::vec3 Position;
    glm::vec3 Velocity;
};


class ParticleSystem : public Component {
public:
    ChoiceProperty ParticleGeometry;
    ResourceProperty<Material> ParticleMaterial;
    DoubleProperty ParticleScale;
    DoubleProperty Mass;
    DoubleProperty Period;
    Vec3Property InitialVelocity;
    Vec3Property ConstantF;
    DoubleProperty DragF;   // Use this for k_d in viscous drag force
    BooleanProperty Billboards;
    // OPTIONAL: Add more UI controls to give more flexibility to your particle system

    ParticleSystem();

    void UpdateModelMatrix(glm::mat4 model_matrix);
    void EmitParticles();
    std::vector<Particle*> GetParticles();
    void StartSimulation();
    void UpdateSimulation(float delta_t, const std::vector<std::pair<SceneObject*, glm::mat4>>& colliders);
    void StopSimulation();
    void ResetSimulation();
    bool IsSimulating();
    bool IsBillboard();

    Signal1<std::string> GeomChanged;

protected:
    // Maximum number of particles in this particle system.
    // For performance reasons, limit the amount of particles to a finite amount.
    static const unsigned int MAX_PARTICLES = 100;

    glm::mat4 model_matrix_;
    double time_to_emit_;
    bool simulating_;
    // OPTIONAL: Add any extra fields as you find necessary

    void OnGeometrySet(int);
};


#endif // PARTICLESYSTEM_H