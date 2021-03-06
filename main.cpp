// ParticleToGridCoord still doesn't handle boundary point -1 and 33

#include <algorithm>        // clamp()
#include <cmath>            // for floor()
#include <cstdlib>          // for rand()
#include <iostream>         // cout, cin
#include <string>           // string
#include <vector>           // vector<T>
#include <typeinfo>

#include "Eigen/Dense"       // for linear algebra operation  compile: -I .
#include "myincludes/tiny_obj_loader.h"    // load and save obj files


using Vec2 = Eigen::Vector2d;
using Vec3 = Eigen::Vector3d;
using Mat2 = Eigen::Matrix2d;


// number of grid boxes
const int N_GRID_BOX = 16;

// width of each grid boxes
const double dx = 1/N_GRID_BOX;

// deltaTime for each simulation step
const double dt = 1e-4;
const double frame_dt = 1e-3;

// snow hardening
const auto particle_mass = 1.0;
const auto vol = 1.0;        // Particle Volume
const auto hardening = 10.0; 
const auto E = 1e4;          // Young's Modulus
const auto nu = 0.2;         // Poisson ratio

// starting lame parameters
const double mu_0 = E / (2 * (1 + nu));
const double lambda_0 = E * nu / ((1+nu) * (1 - 2 * nu));

struct Particle
{
    // velocity waiting for eigen matrix vector 2x1
    Vec2 velocity;
    // position vector 2x1
    Vec2 position;
    // deformation gradient matrix 2x2
    Mat2 F;
    // J = det(F), current volume
    double J;
    // APIC coefficient matrix 2x2
    Mat2 C;
    // need precision (not float)
    double mass;

    Particle() = default;
    Particle(Vec2 t_position, Vec2 t_velocity = Vec2(0, 0), double J = 1)
    : position(t_position), velocity(t_velocity)
    {
        // Eigen initialize format
        F << 1, 1, 1, 1;
        C << 0, 0, 0, 0;
    }
};


// Grid Coord(vx, vy, m)
Vec3 GRID[N_GRID_BOX + 1][N_GRID_BOX + 1];

// set particle data pass by reference -> I want to change particles without returning it
void Initialize(std::vector<Particle> &particles, const std::vector<Vec2> &positions)
{
    for (int index = 0; index < positions.size(); index++)
    {
        // Particle p = Particle(positions[index]);
        particles.push_back(Particle(positions[index]));
    }
}

// This function create particle of Box bounding by bottom_left and top_right
// with each particle spacing is particle_space
std::vector<Vec2> CreateBox(Vec2 bottom_left, Vec2 top_right, double particle_space)
{   
    
    Vec2 nParticle_in_box = Vec2((top_right - bottom_left)/particle_space);
    int xParticle_in_box = floor(nParticle_in_box(0));
    int yParticle_in_box = floor(nParticle_in_box(1));

    std::vector<Vec2> box_Coords;

    for (int i = 0; i <= xParticle_in_box; i++)
    {
        for (int j = 0; j <= yParticle_in_box; j++)
        {
            box_Coords.push_back(bottom_left + Vec2(i, j)*particle_space);        
        }
    }
    return box_Coords;
}

// element-wise power
Vec2 PowVec2(Vec2 vec, int exponent) 
{
    return Vec2(pow(vec[0], exponent), pow(vec[1], exponent));
}

// element-wise floor
Eigen::Vector2i FloorVec2(Vec2 vec) 
{
    return Eigen::Vector2i(static_cast<int>(vec[0]), static_cast<int>(vec[1]));
}

// det of matrix 2x2
double DetMat2(Mat2 mat) 
{
    return mat(0, 0)*mat(1, 1) - mat(1, 0)*mat(0, 1);
}


// This function send info from particle to grid
void ParticleToGrid(const std::vector<Particle> &particles)
{
    for (auto &p: particles) {  
        // Leftmost coord of this particle kernel grid
        Eigen::Vector2i baseCoords = FloorVec2(p.position * N_GRID_BOX - Vec2(0.5, 0.5));
        Vec2 baseCoordToParticleDist = p.position * N_GRID_BOX - baseCoords.cast<double>();

        // Quadratic B-spline
        Vec2 weight[3] = 
        { 
            0.5 * PowVec2(Vec2(1.5, 1.5) - baseCoordToParticleDist, 2),
            Vec2(0.75, 0.75) - PowVec2(baseCoordToParticleDist - Vec2(1.0, 1.0), 2),
            0.5 * PowVec2(baseCoordToParticleDist - Vec2(0.5, 0.5), 2)
        };

        // Lame Parameters
        double e = std::exp(hardening * (1.0f - p.J));
        double mu = mu_0 * e;
        double lambda = lambda_0 * e;
        
        double J = DetMat2(p.F);
        double Dinv = 4*N_GRID_BOX*N_GRID_BOX;

        // R matrix from polar decomposition of F = QR
        Mat2 r = p.F.colPivHouseholderQr().matrixR();

        // fixed corotated material model (other than Neo-Hookean)
        Mat2 rhMat;
        double rhTerm = lambda*(J-1)*J;
        rhMat << rhTerm, rhTerm, rhTerm, rhTerm;
        Mat2 PF = (2*mu*(p.F-r))*p.F.transpose() + rhMat;

        // cauchy stress
        Mat2 stress = - (dt * vol) * (Dinv * PF);

        // APIC affine momentum + MLS-MPM stress
        Mat2 affine = stress + particle_mass * p.C;

        // P2G
        for (int i = 0; i < 3; i++) { 
            for (int j = 0; j < 3; j++) { 
                Vec2 particleToKernelGrid = (Vec2(i, j) - baseCoordToParticleDist) * dx;
                Vec2 momentum = particle_mass * p.velocity;
                Vec3 momentumMass(momentum.x(), momentum.y(), particle_mass);

                // I think this is momentum contribution from moment
                Vec2 affMuldParticle = affine * particleToKernelGrid;
                GRID[baseCoords.x() + i][baseCoords.y() + j] += weight[i].x()*weight[j].y() 
                    * (momentumMass + Vec3(affMuldParticle.x(), affMuldParticle.y(), 0));
            }    
        }
    }
}

void UpdateGrid() {

    for (int i = 0; i < N_GRID_BOX+1; i++) {
        for (int j = 0; j < N_GRID_BOX+1; j++) {
            
            // normalize by mass then add by gravity minus at y coord * dt
            Vec3 &grid = GRID[i][j];
            if (grid[2] > 0) {

                grid /= grid[2]; // divide by mass -> now grid is (vx, vy, 1)
            
                grid += dt * Vec3(0, -200, 0); // gravity

                double boundary = 0.05;
                double nodeCoordsX = static_cast<double>(i) / N_GRID_BOX; 
                double nodeCoordsY = static_cast<double>(j) / N_GRID_BOX;

                // sticky
                if (nodeCoordsX < boundary || nodeCoordsX > 1-boundary || nodeCoordsY > 1-boundary) {
                    grid = Vec3(0, 0, 0);
                }            

                // seperate
                if (nodeCoordsY < boundary) {
                    grid[1] = std::max(0.0, grid[1]);
                }
            }
        }
    }
} 

void GridToParticle(std::vector<Particle> &particles) {

    for (auto &p : particles) {

        // Leftmost coord of this particle kernel grid
        Eigen::Vector2i baseCoords = FloorVec2(p.position * N_GRID_BOX - Vec2(0.5, 0.5));
        Vec2 baseCoordToParticleDist = p.position * N_GRID_BOX - baseCoords.cast<double>();

        // Quadratic B-spline
        Vec2 weight[3] = 
        { 
            0.5 * PowVec2(Vec2(1.5, 1.5) - baseCoordToParticleDist, 2),
            Vec2(0.75, 0.75) - PowVec2(baseCoordToParticleDist - Vec2(1.0, 1.0), 2),
            0.5 * PowVec2(baseCoordToParticleDist - Vec2(0.5, 0.5), 2)
        };

        // ret APIC C and velocity -> we will recalculate these
        Mat2 C(2, 2);
        C << 0, 0, 0, 0;
        Vec2 v(2, 1);
        v << 0, 0;
        p.C = C;
        p.velocity = v;

         // G2P
        for (int i = 0; i < 3; i++) { 
            for (int j = 0; j < 3; j++) { 
                Vec2 particleToKernelGrid = (Vec2(i, j) - baseCoordToParticleDist) * dx;
                Vec2 momentum = particle_mass * p.velocity;
                Vec3 momentumMass(momentum.x(), momentum.y(), particle_mass);

                // send v from kernel grids to particle
                Vec3 GRID_info {GRID[baseCoords.x() + i][baseCoords.y() + j]};
                Vec2 GRID_velocity {GRID_info.x(), GRID_info.y()};    // use uniform initialization instead of copy initialization ..quicker
                double currGridWeight = weight[i].x()*weight[j].y();

                p.velocity += currGridWeight * GRID_velocity;    
                p.C += 4 * N_GRID_BOX*currGridWeight * (GRID_velocity * particleToKernelGrid.transpose());
            }    
        }

        // Advection
        p.position += dt * p.velocity;

        // MLSMPM F update
        Mat2 F = p.F + (dt * p.C) * p.F;

        double oldJ = DetMat2(F);

        // // snow pasticity (comment this out when all simulation is correct)
        // Vec2 sigVec;
        // Mat2 svd_u, svd_v;
        // Eigen::JacobiSVD<Eigen::MatrixXd> svd(F, Eigen::ComputeThinU | Eigen::ComputeThinV);

        // sigVec = svd.singularValues();
        // svd_u = svd.matrixU();
        // svd_v = svd.matrixV();
        // for (int i = 0; i < 2; i++) {
        //     sigVec(i) = std::clamp(sigVec(i), 1.0 - 2.5e-2, 1.0 + 7.5e-3);
        // }

        // Mat2 sigMat;
        // sigMat << sigVec(0), 0, 0, sigVec(1);
        // F = svd_u * sigMat * svd_v.transpose();
        // double newJ = std::clamp(p.J * oldJ / DetMat2(F), 0.6, 20.0);


        // if plasticity p.J = newJ;
        p.J = oldJ;
        p.F = F;
    }
}

void ShowGrid(const Vec3 grid[][N_GRID_BOX+1]) {
    for (int i = 0; i < N_GRID_BOX + 1; i++) {
        std::cout << "Row: " << i << std::endl;
        for (int j = 0; j < N_GRID_BOX + 1; j++) {
            Vec3 temp(grid[i][j]);
            // std::cout << temp.x() << " " << temp.y() << " " << temp.z() << "| ";
            printf("%.3f %.3f %.3f | ", temp.x(), temp.y(), temp.z());
        }
        std::cout << std::endl << "________________________________________________________________" 
            << std::endl << std::endl;
    }
}

void ShowParticle(const std::vector<Particle> &particles, int displayWidth = 12) {

    for (int parIndex = 0; parIndex < particles.size(); parIndex++) {
        auto &p = particles[parIndex];
        printf("%.3f %.3f | ", p.position.x(), p.position.y());
        if (parIndex % displayWidth == displayWidth-1) {
            printf("\n");
        }
    }
    printf("\n");
}

void AdvanceSimulation(std::vector<Particle> &particles) {
    // Reset grid
    std::memset(GRID, 0, sizeof(GRID));

    ParticleToGrid(particles);
    UpdateGrid();
    GridToParticle(particles);
}

int main()
{
    std::vector<Particle> particles;
    
    // create box objects
    // boxCoord, positoin must be in range [0, 1)
    std::vector<Vec2> boxCoords = CreateBox(Vec2(0.5, 0.5), Vec2(0.6, 0.6), 0.01);
    std::vector<Vec2> box2Coords = CreateBox(Vec2(0.45, 0.4), Vec2(0.55, 0.55), 0.01);


    // get box particles info
    Initialize(particles, boxCoords);
    Initialize(particles, box2Coords);


    //BUG no effect in x axis
    std::cout << "Before Update" << std::endl;
    ShowParticle(particles, 11);


    std::cout << "Calculating Simulation... " << std::endl;
    // dt = 1e-4
    for (int i = 0; i < 600; i++) {
        AdvanceSimulation(particles);
    }


    std::cout << "After Update" << std::endl;
    ShowParticle(particles, 11);
    
    // visualize() : TODO

    std::cout << "End of program" << std::endl;
}