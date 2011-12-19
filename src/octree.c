#include <assert.h>
#include <stddef.h>
#include <stdbool.h>

#include <spacepart/scene.h>
#include <spacepart/octree.h>

static int octree_octant(octree_node_t *tree_node, const float *point)
{
    int octant = 0;
    for(int i = 0; i < 3; ++i)
        octant |= (point[i] < tree_node->mid[i]) ? 0 : (1 << i);
    return octant;
}

static int octree_select_octant(octree_node_t *tree_node, const float * restrict aabb_min, const float * restrict aabb_max)
{
    int min_oct = octree_octant(tree_node, aabb_min),
        max_oct = octree_octant(tree_node, aabb_max);
    return (min_oct == max_oct) ? min_oct : -1;
}

static void octree_add_node_root(octree_node_t *tree_node, scene_node_t *scene_node)
{
    assert(scene_node->next == scene_node && scene_node->prev == scene_node);

    scene_node->octree_node = tree_node;
    scene_node->octant = -1;

    tree_node->nodes = scene_join_nodes(tree_node->nodes, scene_node);
    tree_node->num_nodes += 1;
}

static void octree_add_node_child(octree_node_t *tree_node, scene_node_t *scene_node, int octant)
{
    assert(scene_node->next == scene_node && scene_node->prev == scene_node);

    scene_node->octree_node = tree_node;
    scene_node->octant = octant;

    tree_node->child_nodes = scene_join_nodes(tree_node->child_nodes, scene_node);
    tree_node->num_child_nodes += 1;
}

static void octree_add_nodes_child(octree_node_t *tree_node, scene_node_t *node_list, int octant)
{
    if(!node_list) return;

    scene_node_t *node = node_list;
    do
    {
        scene_node_t *next = node->next;
        node->next = node->prev = node;

        octree_add_node_child(tree_node, node, octant);
        node = next;
    } while(node != node_list);
}

static octree_node_t *octree_split_octant(octree_node_t *tree_node, int octant, octree_node_t **free_nodes)
{
    assert(!tree_node->children[octant]);

    assert(*free_nodes); // TODO: handle OOM situations
    octree_node_t *child_node = *free_nodes;
    *free_nodes = child_node->parent;

    tree_node->children[octant] = child_node;
    child_node->parent = tree_node;

    for(int i = 0; i < 3; ++i)
    {
        child_node->min[i] = (octant & (1 << i)) ? tree_node->mid[i] : tree_node->min[i];
        child_node->max[i] = (octant & (1 << i)) ? tree_node->max[i] : tree_node->mid[i];
        child_node->mid[i] = (child_node->min[i] + child_node->max[i]) / 2.0;
    }

    return child_node;
}

static octree_node_t *octree_add_node(octree_node_t *root_node, scene_node_t *scene_node, octree_node_t **free_nodes)
{
    assert(scene_node->next == scene_node && scene_node->prev == scene_node);
    assert(scene_node->octree_node == NULL);

    octree_node_t *node = root_node;
    int octant = -1;

    while(true)
    {
        octant = octree_select_octant(node, scene_node->aabb_min, scene_node->aabb_max);
        if(octant != -1 && node->children[octant]) node = node->children[octant];
        else break;
    }

    if(octant == -1) octree_add_node_root(node, scene_node);
    else if(node->num_child_nodes == -1)
        return octree_add_node(octree_split_octant(node, octant, free_nodes), scene_node, free_nodes);
    else octree_add_node_child(node, scene_node, octant);

    return node;
}

static void octree_split(octree_node_t *tree_node, octree_node_t **free_nodes)
{
    if(!tree_node->child_nodes) return;

    scene_node_t *node = tree_node->child_nodes;
    do
    {
        scene_node_t *next = node->next;
        node->next = node->prev = node;
        node->octree_node = NULL;

        int octant = node->octant;
        node->octant = -1;
        assert(octant != -1);

        octree_node_t *child_node = tree_node->children[octant];
        if(!child_node) child_node = octree_split_octant(tree_node, octant, free_nodes);

        octree_add_node(child_node, node, free_nodes);

        node = next;
    } while(node != tree_node->child_nodes);

    tree_node->child_nodes = NULL;
    tree_node->num_child_nodes = -1;
}

static void octree_remove_node(octree_node_t *tree_node, scene_node_t *scene_node)
{
    if(scene_node->octant == -1) tree_node->num_nodes -= 1;
    else tree_node->num_child_nodes -= 1;

    scene_node_t *next = scene_node->next == scene_node ? NULL : scene_node->next;
    scene_node->prev->next = scene_node->next;
    scene_node->next->prev = scene_node->prev;
    scene_node->octree_node = NULL;
    scene_node->octant = -1;

    if(tree_node->nodes == scene_node) tree_node->nodes = next;
    if(tree_node->child_nodes == scene_node) tree_node->child_nodes = next;
}

static void octree_merge(octree_node_t *tree_node, octree_node_t **free_nodes)
{
    assert(tree_node->num_child_nodes == -1);
    tree_node->num_child_nodes = 0;

    for(int octant = 0; octant < 8; ++octant)
    {
        octree_node_t *child_node = tree_node->children[octant];
        if(!child_node) continue;
        tree_node->children[octant] = NULL;

        assert(child_node->num_child_nodes != -1);

        octree_add_nodes_child(tree_node, child_node->nodes, octant);
        child_node->nodes = NULL;
        child_node->num_nodes = 0;

        octree_add_nodes_child(tree_node, child_node->child_nodes, octant);
        child_node->child_nodes = NULL;
        child_node->num_child_nodes = 0;

        child_node->parent = *free_nodes;
        *free_nodes = child_node;
    }
}

static bool octree_should_merge(octree_node_t *node)
{
    if(node->num_child_nodes != -1)
        return false;

    int nodes_in_children = 0;
    for(int octant = 0; octant < 8; ++octant)
    {
        octree_node_t *child_node = node->children[octant];
        if(!child_node) continue;

        if(child_node->num_child_nodes == -1)
            return false;

        nodes_in_children += child_node->num_nodes;
        nodes_in_children += child_node->num_child_nodes;
    }

    static const int OCTREE_MERGE_THRESHOLD = 4;
    return nodes_in_children < OCTREE_MERGE_THRESHOLD;
}

static bool octree_should_split(octree_node_t *node)
{
    if(node->num_child_nodes == -1) return false;

    // TODO: prevent recursive splits with a sensible heuristic

    static const int OCTREE_SPLIT_THRESHOLD = 8;
    return node->num_child_nodes >= OCTREE_SPLIT_THRESHOLD;
}

void octree_add(octree_node_t *root_node, struct scene_node_t *scene_node, octree_node_t **free_nodes)
{
    octree_node_t *tree_node = octree_add_node(root_node, scene_node, free_nodes);
    if(octree_should_split(tree_node))
        octree_split(tree_node, free_nodes);
}

void octree_remove(struct scene_node_t *scene_node, octree_node_t **free_nodes)
{
    octree_node_t *tree_node = scene_node->octree_node;
    octree_remove_node(tree_node, scene_node);
    if(tree_node->parent && octree_should_merge(tree_node->parent))
        octree_merge(tree_node->parent, free_nodes);
}
