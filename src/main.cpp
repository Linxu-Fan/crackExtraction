

#include "mpm-fracture/mpm-fracture.h"
#include "mpm-fracture/object.h"
#include "mpm-fracture/params.h"
#include "mpm-fracture/extractCrack.h"
#include "mpm-fracture/utils.h" // must be included first because of "#define _USE_MATH_DEFINES" on windows

#include <igl/decimate.h>
#include <nlohmann/json.hpp>
#include <thread>


#include <BulletDynamics/ConstraintSolver/btNNCGConstraintSolver.h>
#include <BulletDynamics/MLCPSolvers/btMLCPSolver.h>
#include <BulletDynamics/MLCPSolvers/btDantzigSolver.h>

///

std::vector<ColliderData> storedData;
std::map<std::string, double> g_rigidBodyVolumes;
std::vector<std::pair<std::string, int>> g_mpmFractureSimTimestepCounts;

std::map<std::string, std::vector<unsigned long long>> g_time_profiles;
std::stack<std::unique_ptr<mini_timer>> g_timestack;

// quick hack to control maximum number of rigid body timesteps in which we should start fracture sim
int stopAfterNFractureInvolvingRbTimesteps = std::numeric_limits<uint32_t>::max();
int numFractureInvolvingRbTimestep = 0;

struct KinematicRBInfo
{
    btRigidBody *rbPtr = nullptr;
    btVector3 linearVelocity = btVector3(0, 0, 0);
    btVector3 angularVelocity = btVector3(0, 0, 0);
};

// We use this callback function to move the kinematic objects of a given scene
void kinematicPreTickCallback(btDynamicsWorld *world, btScalar deltaTime)
{
    std::vector<KinematicRBInfo> *kinematicRBs = reinterpret_cast<std::vector<KinematicRBInfo> *>(world->getWorldUserInfo());
    ASSERT(kinematicRBs != nullptr);

    // for each kinematic object in the list
    for (std::vector<KinematicRBInfo>::iterator rbit = kinematicRBs->begin(); rbit != kinematicRBs->end(); ++rbit)
    {
        btRigidBody *rb = rbit->rbPtr;
        const btVector3 &linearVelocity = rbit->linearVelocity;
        const btVector3 &angularVelocity = rbit->angularVelocity;

        // calculate the predicted transform for current kinematic object
        btTransform predictedTrans;
        btTransformUtil::integrateTransform(rb->getWorldTransform(), linearVelocity, angularVelocity, deltaTime, predictedTrans);

        rb->getMotionState()->setWorldTransform(predictedTrans);
    }
}

/**@brief Get the euler angles from this quaternion
 * @param yaw Angle around Z
 * @param pitch Angle around Y
 * @param roll Angle around X */
void getEulerZYX(btScalar &yawZ, btScalar &pitchY, btScalar &rollX, const btQuaternion &q)
{
    btScalar squ;
    btScalar sqx;
    btScalar sqy;
    btScalar sqz;
    btScalar sarg;
    sqx = q.x() * q.x();
    sqy = q.y() * q.y();
    sqz = q.z() * q.z();
    squ = q.w() * q.w();
    rollX = btAtan2(2 * (q.y() * q.z() + q.w() * q.x()), squ - sqx - sqy + sqz);
    sarg = btScalar(-2.) * (q.x() * q.z() - q.w() * q.y());
    pitchY = sarg <= btScalar(-1.0) ? btScalar(-0.5) * SIMD_PI : (sarg >= btScalar(1.0) ? btScalar(0.5) * SIMD_PI : btAsin(sarg));
    yawZ = btAtan2(2 * (q.x() * q.y() + q.w() * q.z()), squ + sqx - sqy - sqz);
}




// find the surface mesh from a vdb grid
void getSurfaceMeshFromVdbGrid(openvdb::FloatGrid::Ptr bareMeshVdbGridPtr,int decimateTarget, meshObjFormat& fragmentVolume)
{
    openvdb::tools::VolumeToMesh volumeToMeshHandle;
    volumeToMeshHandle(*bareMeshVdbGridPtr);

    trimesh::TriMesh* pMesh = new trimesh::TriMesh;

    openvdb::tools::PointList *verts = &volumeToMeshHandle.pointList();
    openvdb::tools::PolygonPoolList *polys = &volumeToMeshHandle.polygonPoolList();


    for (size_t i = 0; i < volumeToMeshHandle.pointListSize(); i++)
    {
        openvdb::Vec3s &v = (*verts)[i];
        // NOTE: This is a hack! The y coord is negated because of mixup with blender's XZY coordinate system ****************************************************************************************
        pMesh->vertices.push_back(trimesh::vec3(v[0], v[1], v[2]));

    }

    for (size_t i = 0; i < volumeToMeshHandle.polygonPoolListSize(); i++)
    {

        for (size_t ndx = 0; ndx < (*polys)[i].numTriangles(); ndx++)
        {
            openvdb::Vec3I *p = &((*polys)[i].triangle(ndx));
        }

        for (size_t ndx = 0; ndx < (*polys)[i].numQuads(); ndx++)
        {
            openvdb::Vec4I *p = &((*polys)[i].quad(ndx));

            trimesh::TriMesh::Face f0;
            f0[0] = p->z();
            f0[1] = p->y();
            f0[2] = p->x();
            trimesh::TriMesh::Face f1;
            f1[0] = p->w();
            f1[1] = p->z();
            f1[2] = p->x();

            pMesh->faces.push_back(f0);
            pMesh->faces.push_back(f1);

        }
    }


    // decimate mesh using libigl
    Eigen::MatrixXd V(pMesh->vertices.size(), 3);
    Eigen::MatrixXi F(pMesh->faces.size(), 3);
    for(int i = 0; i < pMesh->vertices.size(); i++)
    {
        V(i,0) = pMesh->vertices[i][0];
        V(i,1) = pMesh->vertices[i][1];
        V(i,2) = pMesh->vertices[i][2];
    }
    for(int i = 0; i < pMesh->faces.size(); i++)
    {
        F(i,0) = pMesh->faces[i][0];
        F(i,1) = pMesh->faces[i][1];
        F(i,2) = pMesh->faces[i][2];
    }



    Eigen::MatrixXd U;
    Eigen::MatrixXi G;
    Eigen::VectorXi J;
    Eigen::VectorXi I;
    igl::decimate(V, F, decimateTarget, U, G, J, I);
    for (int i = 0; i < U.rows(); ++i)
    {
        Eigen::Vector3d p = {U(i, 0), U(i, 1), U(i, 2)};
        fragmentVolume.vertices.push_back(p);
    }
    for (int i = 0; i < G.rows(); ++i)
    {
        std::vector<int> f;
        f.push_back(G(i, 0));
        f.push_back(G(i, 1));
        f.push_back(G(i, 2));
        fragmentVolume.faces.push_back(f);
    }




}


// the input particles may locate in the negative domain. This function projects them to the positive domain
void preprocessing(std::string crackFilePath, std::string cutObjectFilePath, parametersSim* parameters, std::vector<Particle>* particleVec, meshObjFormat* objectMesh)
{
    // project damaged particles to the positive domain
    // read damaged particles
    std::ifstream inf;
    inf.open(crackFilePath);
    std::string sline;
    std::string s0, s1, s2, s3;
    while (getline(inf, sline)) {

        std::istringstream sin(sline);
        sin >> s0 >> s1 >> s2 >> s3;
        Eigen::Vector3d ipos = { atof(s0.c_str()), atof(s1.c_str()), atof(s2.c_str()) };
        Eigen::Vector3d ivel = { 0, 0, 0 };
        double iDp = atof(s3.c_str());
        (*particleVec).push_back(Particle(ipos, ivel, 0, 0, iDp));
    }
    double xmin = (*particleVec)[0].pos[0], xmax = (*particleVec)[0].pos[0], ymin = (*particleVec)[0].pos[1], ymax = (*particleVec)[0].pos[1], zmin = (*particleVec)[0].pos[2], zmax = (*particleVec)[0].pos[2];
    for (int km = 0; km < (*particleVec).size(); km++) 
    {
        Eigen::Vector3d ipos = (*particleVec)[km].pos;
        xmin = std::min(xmin, ipos[0]);
        xmax = std::max(xmax, ipos[0]);
        ymin = std::min(ymin, ipos[1]);
        ymax = std::max(ymax, ipos[1]);
        zmin = std::min(zmin, ipos[2]);
        zmax = std::max(zmax, ipos[2]);
    }
    Eigen::Vector3d minCoordinate = { xmin - 10 * (*parameters).dx, ymin - 10 * (*parameters).dx, zmin - 10 * (*parameters).dx };
    for (int km = 0; km < (*particleVec).size(); km++) {
        (*particleVec)[km].pos = (*particleVec)[km].pos - minCoordinate;
    }
    Eigen::Vector3d length = {xmax - xmin + 20* (*parameters).dx,  ymax - ymin + 20* (*parameters).dx, zmax - zmin + 20 * (*parameters).dx};
    (*parameters).length = length;
    (*parameters).minCoordinate = minCoordinate;


    // read cutting object's mesh
    meshObjFormat objectMeshTmp = readObj(cutObjectFilePath);
    (*objectMesh).vertices = objectMeshTmp.vertices;
    for(int m = 0; m < (*objectMesh).vertices.size(); m++)
    {
        (*objectMesh).vertices[m] = (*objectMesh).vertices[m] - minCoordinate;
    }
    (*objectMesh).faces = objectMeshTmp.faces;
}


// the input particles may locate in the negative domain. This function projects them to the positive domain
void postprocessing(parametersSim* parameters, std::tuple<bool, meshObjFormat,  meshObjFormat, std::vector<meshObjFormat> >* crackSurface, meshObjFormat* objectMesh)
{
    // project damaged particles to the original domain
    for(int m = 0; m < std::get<1>(*crackSurface).vertices.size(); m++)
    {
        std::get<1>(*crackSurface).vertices[m] = std::get<1>(*crackSurface).vertices[m] + (*parameters).minCoordinate;
    }


    for(int m = 0; m < std::get<2>(*crackSurface).vertices.size(); m++)
    {
        std::get<2>(*crackSurface).vertices[m] = std::get<2>(*crackSurface).vertices[m] + (*parameters).minCoordinate;
    }


    for(int m = 0; m < std::get<3>(*crackSurface).size(); m++)
    {
        for(int n = 0; n < std::get<3>(*crackSurface)[m].vertices.size(); n++)
        {
            std::get<3>(*crackSurface)[m].vertices[n] = std::get<3>(*crackSurface)[m].vertices[n] + (*parameters).minCoordinate;
        }
    }


    // project cutting objects to the original domain
    for(int m = 0; m < (*objectMesh).vertices.size(); m++)
    {
        (*objectMesh).vertices[m] = (*objectMesh).vertices[m] + (*parameters).minCoordinate;
    }

}


#include <igl/read_triangle_mesh.h>
#include <igl/point_mesh_squared_distance.h>
#include <igl/per_face_normals.h>
#include <igl/AABB.h>
#include <igl/point_mesh_squared_distance.h>

int main(int argc, char *argv[])
{
    // the resolution of crack faces
    double dx = 0.01;

    // parameters
    parametersSim parameters;
    parameters.dx = 0.002; 
    parameters.vdbVoxelSize = 0.0002;
    std::string crackFilePath = "/home/floyd/Linxu/clearCode/mpm-fracture/build/output/particles4500.txt";
    std::string cutObjectFilePath = "/home/floyd/Linxu/clearCode/mpm-fracture/build/output/fixed__sf.obj";
    std::vector<Particle> particleVec;
    meshObjFormat objectMesh;

    // extract the crack surface
    preprocessing(crackFilePath, cutObjectFilePath, &parameters, &particleVec, &objectMesh); 
    std::tuple<bool, meshObjFormat,  meshObjFormat, std::vector<meshObjFormat> > result =  extractCrackSurface(&particleVec, parameters);
    postprocessing(&parameters, &result, &objectMesh);


    // output the crack surface and fragments
    meshObjFormat crackSurfacePartialCut = std::get<1>(result);
    writeObjFile(crackSurfacePartialCut.vertices, crackSurfacePartialCut.faces, "./output/partialCutSurface");
    meshObjFormat crackSurfaceFullCut = std::get<2>(result);
    writeObjFile(crackSurfaceFullCut.vertices, crackSurfaceFullCut.faces, "./output/fullCutSurface");
    std::vector<meshObjFormat> fragments = std::get<3>(result);
    for(int i = 0; i < fragments.size(); i++)
    {
        writeObjFile(fragments[i].vertices, fragments[i].faces, "./output/fragment_"+std::to_string(i));
    }
    writeObjFile(objectMesh.vertices, objectMesh.faces, "./output/object");


    // define the cutting method
    // case 0: complete cut with MCUT
    // case 1: complete cut with openVDB
    // case 2: partial cut with openVDB
    int cuttingMethod = 0;
    switch(cuttingMethod)
    {

        case 0:
        {
            std::vector<meshObjFormat> fragmentsFinal;
            cutObject_MCUT(parameters,"tmpCutPbject", &objectMesh, &fragments, &fragmentsFinal);

            // for each fragment level
            for (unsigned int i = 0; i < fragmentsFinal.size(); ++i) 
            {
                writeObjFile(fragmentsFinal[i].vertices, fragmentsFinal[i].faces, "./output/fullCut_MCUT_Fragment_"+std::to_string(i));
            }
        }
        break;

        case 1:
        {
            // define openvdb linear transformation
            openvdb::math::Transform::Ptr transform = openvdb::math::Transform::createLinearTransform(parameters.vdbVoxelSize);

            
            // convert crack surface mesh to vdb grid
            GenericMesh crackSurfaceGeneric;
            crackSurfaceGeneric.m.vertices = crackSurfaceFullCut.vertices;
            crackSurfaceGeneric.m.faces = crackSurfaceFullCut.faces;
            triangulateGenericMesh(crackSurfaceGeneric);


            std::vector<openvdb::Vec3f> myMeshPoints_crack;
            for (int i = 0; i < crackSurfaceGeneric.m.vertices.size(); ++i) 
            {
                const Eigen::Vector3d p = crackSurfaceGeneric.m.vertices[i];
                myMeshPoints_crack.push_back(openvdb::Vec3f(p[0], p[1], p[2]));
            }
            std::vector<openvdb::Vec3I> myMeshTris_crack;
            for (int i = 0; i < crackSurfaceGeneric.triangulatedFaces.size() / 3; ++i) 
            {
                myMeshTris_crack.push_back(openvdb::Vec3I(crackSurfaceGeneric.triangulatedFaces[3 * i + 0], crackSurfaceGeneric.triangulatedFaces[3 * i + 1], crackSurfaceGeneric.triangulatedFaces[3 * i + 2]));
            }



            openvdb::FloatGrid::Ptr crackLevelSetGrid = openvdb::tools::meshToUnsignedDistanceField<openvdb::FloatGrid>(
                *transform,
                myMeshPoints_crack,
                myMeshTris_crack,
                std::vector<openvdb::Vec4I>(),
                3);

            for (openvdb::FloatGrid::ValueOnIter iter = crackLevelSetGrid->beginValueOn(); iter; ++iter) {
                float dist = iter.getValue();
                float value = dist - std::sqrt(3 * std::pow(parameters.vdbVoxelSize, 2));
                iter.setValue(value);
            }
            crackLevelSetGrid->setGridClass(openvdb::GRID_LEVEL_SET);




            // convert object mesh to vdb grid
            GenericMesh objectMeshGeneric;
            objectMeshGeneric.m.vertices = objectMesh.vertices;
            objectMeshGeneric.m.faces = objectMesh.faces;
            triangulateGenericMesh(objectMeshGeneric);


            std::vector<openvdb::Vec3f> myMeshPoints_object;
            for (int i = 0; i < objectMeshGeneric.m.vertices.size(); ++i) 
            {
                const Eigen::Vector3d p = objectMeshGeneric.m.vertices[i];
                myMeshPoints_object.push_back(openvdb::Vec3f(p[0], p[1], p[2]));
            }
            std::vector<openvdb::Vec3I> myMeshTris_object;
            for (int i = 0; i < objectMeshGeneric.triangulatedFaces.size() / 3; ++i) 
            {
                myMeshTris_object.push_back(openvdb::Vec3I(objectMeshGeneric.triangulatedFaces[3 * i + 0], objectMeshGeneric.triangulatedFaces[3 * i + 1], objectMeshGeneric.triangulatedFaces[3 * i + 2]));
            }


            // create a float grid containing the level representation of the sphere mesh
            openvdb::FloatGrid::Ptr objectLevelSetGrid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
                *transform,
                myMeshPoints_object,
                myMeshTris_object);
            objectLevelSetGrid->setGridClass(openvdb::GRID_LEVEL_SET);



            // do the boolean operation
            openvdb::FloatGrid::Ptr copyOfCrackGrid = crackLevelSetGrid->deepCopy();
            openvdb::FloatGrid::Ptr copyOfObjGrid = objectLevelSetGrid->deepCopy();
            // Compute the difference (A / B) of the two level sets.
            openvdb::tools::csgDifference(*copyOfObjGrid, *copyOfCrackGrid);
            openvdb::FloatGrid::Ptr csgSubtractedObjGrid = copyOfObjGrid; // cutted piece
            // list of fragment pieces after cutting (as level set grids)
            std::vector<openvdb::FloatGrid::Ptr> fragmentLevelSetGridPtrList;
            openvdb::tools::segmentSDF(*csgSubtractedObjGrid, fragmentLevelSetGridPtrList);




            // for each fragment level
            for (unsigned int i = 0; i < fragmentLevelSetGridPtrList.size(); ++i) 
            {
                meshObjFormat fullCutFragment;
                getSurfaceMeshFromVdbGrid(fragmentLevelSetGridPtrList[i],1000000, fullCutFragment);
                writeObjFile(fullCutFragment.vertices, fullCutFragment.faces, "./output/fullCutFragment_"+std::to_string(i));
            }

        
        }
        break;

        case 2:
        {
            // define openvdb linear transformation
            openvdb::math::Transform::Ptr transform = openvdb::math::Transform::createLinearTransform(parameters.vdbVoxelSize);

            
            // convert crack surface mesh to vdb grid
            GenericMesh crackSurfaceGeneric;
            crackSurfaceGeneric.m.vertices = crackSurfacePartialCut.vertices;
            crackSurfaceGeneric.m.faces = crackSurfacePartialCut.faces;
            triangulateGenericMesh(crackSurfaceGeneric);


            std::vector<openvdb::Vec3f> myMeshPoints_crack;
            for (int i = 0; i < crackSurfaceGeneric.m.vertices.size(); ++i) 
            {
                const Eigen::Vector3d p = crackSurfaceGeneric.m.vertices[i];
                myMeshPoints_crack.push_back(openvdb::Vec3f(p[0], p[1], p[2]));
            }
            std::vector<openvdb::Vec3I> myMeshTris_crack;
            for (int i = 0; i < crackSurfaceGeneric.triangulatedFaces.size() / 3; ++i) 
            {
                myMeshTris_crack.push_back(openvdb::Vec3I(crackSurfaceGeneric.triangulatedFaces[3 * i + 0], crackSurfaceGeneric.triangulatedFaces[3 * i + 1], crackSurfaceGeneric.triangulatedFaces[3 * i + 2]));
            }



            openvdb::FloatGrid::Ptr crackLevelSetGrid = openvdb::tools::meshToUnsignedDistanceField<openvdb::FloatGrid>(
                *transform,
                myMeshPoints_crack,
                myMeshTris_crack,
                std::vector<openvdb::Vec4I>(),
                3);

            for (openvdb::FloatGrid::ValueOnIter iter = crackLevelSetGrid->beginValueOn(); iter; ++iter) {
                float dist = iter.getValue();
                float value = dist - std::sqrt(3 * std::pow(parameters.vdbVoxelSize, 2));
                iter.setValue(value);
            }
            crackLevelSetGrid->setGridClass(openvdb::GRID_LEVEL_SET);




            // convert object mesh to vdb grid
            GenericMesh objectMeshGeneric;
            objectMeshGeneric.m.vertices = objectMesh.vertices;
            objectMeshGeneric.m.faces = objectMesh.faces;
            triangulateGenericMesh(objectMeshGeneric);


            std::vector<openvdb::Vec3f> myMeshPoints_object;
            for (int i = 0; i < objectMeshGeneric.m.vertices.size(); ++i) 
            {
                const Eigen::Vector3d p = objectMeshGeneric.m.vertices[i];
                myMeshPoints_object.push_back(openvdb::Vec3f(p[0], p[1], p[2]));
            }
            std::vector<openvdb::Vec3I> myMeshTris_object;
            for (int i = 0; i < objectMeshGeneric.triangulatedFaces.size() / 3; ++i) 
            {
                myMeshTris_object.push_back(openvdb::Vec3I(objectMeshGeneric.triangulatedFaces[3 * i + 0], objectMeshGeneric.triangulatedFaces[3 * i + 1], objectMeshGeneric.triangulatedFaces[3 * i + 2]));
            }


            // create a float grid containing the level representation of the sphere mesh
            openvdb::FloatGrid::Ptr objectLevelSetGrid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
                *transform,
                myMeshPoints_object,
                myMeshTris_object);
            objectLevelSetGrid->setGridClass(openvdb::GRID_LEVEL_SET);



            // do the boolean operation
            openvdb::FloatGrid::Ptr copyOfCrackGrid = crackLevelSetGrid->deepCopy();
            openvdb::FloatGrid::Ptr copyOfObjGrid = objectLevelSetGrid->deepCopy();
            // Compute the difference (A / B) of the two level sets.
            openvdb::tools::csgDifference(*copyOfObjGrid, *copyOfCrackGrid);
            openvdb::FloatGrid::Ptr csgSubtractedObjGrid = copyOfObjGrid; // cutted piece
            // list of fragment pieces after cutting (as level set grids)
            std::vector<openvdb::FloatGrid::Ptr> fragmentLevelSetGridPtrList;
            openvdb::tools::segmentSDF(*csgSubtractedObjGrid, fragmentLevelSetGridPtrList);




            // for each fragment level
            for (unsigned int i = 0; i < fragmentLevelSetGridPtrList.size(); ++i) 
            {
                meshObjFormat fullCutFragment;
                getSurfaceMeshFromVdbGrid(fragmentLevelSetGridPtrList[i],1000000, fullCutFragment);
                writeObjFile(fullCutFragment.vertices, fullCutFragment.faces, "./output/partialCutFragment_"+std::to_string(i));
            }
        }
        break;

    }
    

    
    return 0;
}