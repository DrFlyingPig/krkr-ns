#include "EmotePlaybackCompletion.h"
#include <iostream>
#include <vector>

struct Node { bool isParameterize = false; };
struct Motion {
    int loopTime = -1;
    int lastTime = 182;
    bool isParameterize = false;
    std::vector<Node*> nodeList;
    std::vector<Motion*> children;
};

int main()
{
    int checks = 0;
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        ++checks;
        if (!value) { ++failures; std::cerr << message << '\n'; }
    };
    auto finite = [](Motion* root) {
        return emoteplayer::IsFinitePlaybackGraph(root,
            [](Motion* m) { return emoteplayer::IsFiniteMotion(m); },
            [](Motion* m) { return m->children; });
    };
    using emoteplayer::HasCompletedFinitePlayback;
    Motion root, child, leaf;
    root.children = {&child};
    child.children = {&leaf};
    check(finite(&root), "finite referenced title graph");
    check(!HasCompletedFinitePlayback(true, finite(&root), false, false), "running main is active");
    check(HasCompletedFinitePlayback(false, finite(&root), false, false), "natural finite completion");
    check(!HasCompletedFinitePlayback(false, finite(&root), true, false), "parameterized eye control remains active");
    root.loopTime = 0;
    check(!finite(&root), "root whole-motion loop");
    root.loopTime = 30;
    check(!finite(&root), "root partial loop");
    root.loopTime = -1;
    leaf.loopTime = 0;
    check(!finite(&root), "finite parent retains looping child");
    check(!HasCompletedFinitePlayback(false, finite(&root), false, false), "looping child blocks aggregate completion");
    leaf.loopTime = -1;
    child.isParameterize = true;
    check(!finite(&root), "parameterized child blocks aggregate completion");
    child.isParameterize = false;
    root.isParameterize = true;
    check(!finite(&root), "parameterized motion without metadata variables");
    root.isParameterize = false;
    Node parameterizedNode{true};
    child.nodeList = {&parameterizedNode};
    check(!finite(&root), "parameterized child node blocks completion");
    child.nodeList.clear();
    leaf.lastTime = -1;
    check(!finite(&root), "unbounded child blocks completion");
    leaf.lastTime = 182;
    leaf.children = {&root};
    check(!finite(&root), "reference cycle is conservative");
    leaf.children.clear();
    root.children = {&child, &leaf};
    check(finite(&root), "shared child is not a cycle");
    root.children.push_back(&child);
    check(finite(&root), "repeated reference is not a cycle");
    root.children.push_back(nullptr);
    check(!finite(&root), "unresolved reference is conservative");
    root.children.pop_back();
    check(!finite(nullptr), "missing selected motion");
    // The completion guard treats both parallel and difference timelines as
    // active. Their existing progression remains responsible for termination.
    check(!HasCompletedFinitePlayback(false, finite(&root), false, true), "active parallel/difference timeline blocks completion");
    check(HasCompletedFinitePlayback(false, finite(&root), false, false), "terminated timeline permits finite completion");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
