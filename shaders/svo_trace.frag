#version 450

layout(location = 0) in vec3 texCoords;

layout(push_constant) uniform PushConstantBlock {
	mat4 NDCtoWorld;
	vec3 cameraPos;
	int maxIterations;
	vec3 skyColor;
	int debugColor;
} pc;

layout(binding = 0) uniform sampler3D brick_sampler;

layout(location = 0) out vec4 outColor;

float piOver2 = asin(1.0);
vec4 horizonColor = vec4(0.8,0.9,1.0, 1.0);
vec3 octree_location = vec3(-1.0,-1.0,-1.0);
float octree_scale = 2.0;
int brick_size = 64;

vec4 missColor(vec3 direction){
    float dotProd = dot(direction, vec3(0.0,0.0,1.0));
    dotProd = clamp(dotProd, -1.0, 1.0);
    float theta = acos(dotProd) / piOver2;

    vec4 sky = vec4(pc.skyColor, 1.0);

    if(theta < 1){
        return sky*(1-theta) + horizonColor*theta;
    }
    else{
	    return horizonColor*(2-theta);
    }
}

void main() {
     vec3 rayOrigin = pc.cameraPos;
	vec4 transformed = pc.NDCtoWorld * vec4(texCoords, 1.0);
    vec3 pixelLocation = transformed.xyz / transformed.w;
	vec3 direction = normalize(pixelLocation-rayOrigin);

    vec3 invDir = 1.0 / direction;
    vec3 tMin = (-1.0 - rayOrigin) * invDir;
    vec3 tMax = (1.0 - rayOrigin) * invDir;

    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);

    float tEntry = max(max(t1.x, t1.y), t1.z);
    float tExit = min(min(t2.x, t2.y), t2.z);

    bvec3 step_direction = bvec3(tEntry == t1.x, tEntry == t1.y, tEntry == t1.z);
    if (tEntry <= tExit && tExit >= 0.0){

        bvec3 advance = bvec3(direction.x >= 0, direction.y >= 0, direction.z >= 0);

        vec3 world_point = rayOrigin + direction * (tEntry+0.001);
        if(tEntry < 0.0) world_point = rayOrigin;


        vec3 octree_point = (world_point - octree_location) / octree_scale;
        vec3 voxel_point = octree_point*float(brick_size);
        ivec3 voxel_coord = ivec3(floor(voxel_point));

        int i = 0;
        while(i < pc.maxIterations){


            if(voxel_coord.x < 0 || voxel_coord.y < 0 || voxel_coord.z < 0 || voxel_coord.x >= brick_size || voxel_coord.y >= brick_size || voxel_coord.z >= brick_size){
                vec4 miss = missColor(direction);
                if(pc.debugColor != 0) miss -= vec4(vec3(i / 250.0), 0.0);
				outColor = miss;
                return;
			}

            vec4 voxel = texelFetch(brick_sampler, voxel_coord, 0);
            if(voxel != vec4(0.0)){
                vec4 hit = vec4(vec3(1.0)-vec3(step_direction)*0.1, 1.0);
                if(pc.debugColor != 0) hit -= vec4(vec3(i / 250.0), 0.0);
                outColor = hit;
				return;
			}

            tMin = (voxel_coord - voxel_point) * invDir;
            tMax = (voxel_coord + 1.0 - voxel_point) * invDir;
            t1 = min(tMin, tMax);
            t2 = max(tMin, tMax);
            tExit = min(min(t2.x, t2.y), t2.z);
            step_direction = bvec3(tExit == t2.x, tExit == t2.y, tExit == t2.z);
            voxel_coord += ivec3(step_direction) * (ivec3(-1)+2*ivec3(advance));

            voxel_point = voxel_point + direction * tExit;
            i++;
        }
    }
	else{
        outColor = missColor(direction);
	}
}
