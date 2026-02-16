#pragma once
#include <vector>
#include <memory>

class Octree
{
private:

public:

	static std::shared_ptr<Octree> CreateOctree(int brick_size);
};

