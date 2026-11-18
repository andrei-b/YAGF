#include "regiongeometry.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using yagf::core::constrainRegion;

int main()
{
    const QRectF start(10, 10, 20, 20);
    const QVector<QRectF> right{{40, 0, 20, 100}};
    assert(constrainRegion(start, {100, 10, 20, 20}, right) == QRect(20, 10, 20, 20));
    assert(constrainRegion(start, {10, 10, 100, 20}, right) == QRect(10, 10, 30, 20));
    assert(constrainRegion(start, {10, 100, 20, 20}, {{0, 40, 100, 20}}) == QRect(10, 20, 20, 20));
    assert(constrainRegion(start, {10, 10, 20, 100}, {{0, 40, 100, 20}}) == QRect(10, 10, 20, 30));
    assert(constrainRegion(start, {-100, 10, 20, 20}, {{-20, 0, 20, 100}}) == QRect(0, 10, 20, 20));
    assert(constrainRegion(start, {-100, 10, 130, 20}, {{-20, 0, 20, 100}}) == QRect(0, 10, 30, 20));
    assert(constrainRegion(start, {10, -100, 20, 20}, {{0, -20, 100, 20}}) == QRect(10, 0, 20, 20));
    assert(constrainRegion(start, {10, -100, 20, 130}, {{0, -20, 100, 20}}) == QRect(10, 0, 20, 30));
    // Swept collision prevents jumping entirely over a thin block in one event.
    assert(constrainRegion(start, {150, 10, 20, 20}, {{40, 0, 1, 100}}) == QRect(20, 10, 20, 20));
    assert(constrainRegion(start, {100, 100, 20, 20}, {{40, 40, 20, 20}}) == QRect(20, 20, 20, 20));
    assert(constrainRegion(start, {10, 10, 100, 100}, {{40, 40, 20, 20}}) == QRect(10, 10, 30, 30));
    // Touching is legal; sliding along an edge and moving away must remain possible.
    assert(constrainRegion(start, {10, 60, 20, 20}, {{30, 0, 20, 100}}) == QRect(10, 60, 20, 20));
    assert(constrainRegion(start, {0, 10, 20, 20}, {{30, 0, 20, 100}}) == QRect(0, 10, 20, 20));
    assert(constrainRegion(start, {100, 10, 20, 20}, {{90, 0, 10, 100}, {40, 0, 10, 100}}) == QRect(20, 10, 20, 20));
    // Existing overlaps in older projects can be separated.
    assert(constrainRegion(start, {100, 10, 20, 20}, {{15, 15, 20, 20}}) == QRect(100, 10, 20, 20));
    // New blocks grow from an empty anchor and keep the first permitted boundary.
    assert(constrainRegion({10, 10, 0, 0}, {10, 10, 100, 40}, {{50, 0, 20, 100}}) == QRect(10, 10, 40, 16));
    assert(constrainRegion({90, 10, 0, 0}, {-10, 10, 100, 40}, {{30, 0, 20, 100}}) == QRect(50, 10, 40, 16));
    assert(constrainRegion({10, 10, 0, 0}, {10, 10, 40, 100}, {{0, 50, 100, 20}}) == QRect(10, 10, 16, 40));
    assert(constrainRegion({10, 90, 0, 0}, {10, -10, 40, 100}, {{0, 30, 100, 20}}) == QRect(10, 50, 16, 40));
}
