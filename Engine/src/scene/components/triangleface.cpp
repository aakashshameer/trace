#include "triangleface.h"

TriangleFace::TriangleFace(glm::vec3 a_, glm::vec3 b_, glm::vec3 c_, glm::vec3 a_n_, glm::vec3 b_n_, glm::vec3 c_n_, glm::vec2 a_uv_, glm::vec2 b_uv_, glm::vec2 c_uv_, bool use_per_vertex_normals_) :
    a(a_), b(b_), c(c_), a_n(a_n_), b_n(b_n_), c_n(c_n_), a_uv(a_uv_), b_uv(b_uv_), c_uv(c_uv_), use_per_vertex_normals(use_per_vertex_normals_)
{
    local_bbox.reset(new BoundingBox(glm::min(a,glm::min(b,c)),glm::max(a,glm::max(b,c))));
}

bool TriangleFace::IntersectLocal(const Ray &r, Intersection &i)
{
   // REQUIREMENT: Add triangle intersection code here.
   // it currently ignores all triangles and just returns false.
   //
   // Note that you are only intersecting a single triangle, and the vertices
   // of the triangle are supplied to you by the trimesh class.
   //
   // use_per_vertex_normals tells you if the triangle has per-vertex normals.
   // If it does, you should compute and use the Phong-interpolated normal at the intersection point.
   // If it does not, you should use the normal of the triangle's supporting plane.
   //
   // If the ray r intersects the triangle abc:
   // 1. put the hit parameter in i.t
   // 2. put the normal in i.normal
   // 3. put the texture coordinates in i.uv
   // and return true;

    glm::dvec3 n = cross((b - a),(c-a)) / length(cross((b - a),(c-a)));
    glm::dvec3 d = r.direction;
    glm::dvec3 e = r.position;
    double k = dot(n,a);

    if ( dot(n,d) == 0) {
        return false;
    }

    double t = (k - dot(n,e)) / dot(n,d);
    if (t < 0) {
        return false;
    }

    glm::dvec3 q = e + t * d;

    bool inside_ab = dot(cross(b-a,q-a),n) >= 0;
    bool inside_bc = dot(cross(c-b,q-b),n) >= 0;
    bool inside_ca = dot(cross(a-c,q-c),n) >= 0;

    if ( inside_ab && inside_bc && inside_ca) {
         i.t = t + NORMAL_EPSILON;

         double deno = dot(cross(b-a,c-a),n);
         float alpha = dot(cross(c-b,q-b),n) / deno;
         float beta = dot(cross(a-c,q-c),n) / deno;
         float gamma = dot(cross(b-a,q-a),n) / deno;

        if (use_per_vertex_normals) {
            glm::dvec3 norm_q = ((alpha * a_n) + (beta * b_n) + (gamma * c_n)) /
                    length((alpha * a_n) + (beta * b_n) + (gamma * c_n));

            i.normal = norm_q;
        } else {
            i.normal = n;
        }

        glm::dvec2 uv_q = ((alpha * a_uv) + (beta * b_uv) + (gamma * c_uv));
        i.uv = uv_q;

        return true;
    }

   return false;
}
