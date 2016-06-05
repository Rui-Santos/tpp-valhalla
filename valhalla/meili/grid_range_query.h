// -*- mode: c++ -*-
#ifndef MMP_GRID_RANGE_QUERY_H_
#define MMP_GRID_RANGE_QUERY_H_

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>
#include <unordered_set>
#include <cmath>
#include <cassert>
#include <stdexcept>

#include <valhalla/midgard/aabb2.h>
#include <valhalla/midgard/pointll.h>
#include <valhalla/midgard/linesegment2.h>


namespace valhalla{

namespace meili {

using Point = valhalla::midgard::PointLL;
using LineSegment = valhalla::midgard::LineSegment2<Point>;
using BoundingBox = valhalla::midgard::AABB2<Point>;

// Represents one intersection beetween one side of a bounding box and a segment
struct BoundingBoxIntersection {
  Point point;  // The intersection point
  int dx, dy;   // The direction to the cell adjacent to the intersected side
};


template <typename key_t>
class GridRangeQuery
{
 public:
  GridRangeQuery() = delete;

  GridRangeQuery(const BoundingBox& bbox, float cell_width, float cell_height) {
    if (cell_width <= 0.f) {
      throw std::invalid_argument("invalid cell width (require positive width)");
    }
    if (cell_height <= 0.f) {
      throw std::invalid_argument("invalid cell height (require positive height)");
    }
    auto bbox_width = bbox.Width();
    if (bbox_width <= 0.f) {
      throw std::invalid_argument("invalid bounding box (require positive width)");
    }
    auto bbox_height = bbox.Height();
    if (bbox_height <= 0.f) {
      throw std::invalid_argument("invalid bounding box (require positive height)");
    }
    bbox_ = bbox;
    cell_width_ = std::min(bbox_width, cell_width);
    cell_height_ = std::min(bbox_height, cell_height);
    num_rows_ = ceil(bbox_width / cell_width_);
    num_cols_ = ceil(bbox_height / cell_height_);
    items_.resize(num_cols_ * num_rows_);
  }

  const BoundingBox& bbox() const {
    return bbox_;
  }

  int num_rows() const {
    return num_rows_;
  }

  int num_cols() const {
    return num_cols_;
  }

  float cell_width() const {
    return cell_width_;
  }

  float cell_height() const {
    return cell_height_;
  }

  std::pair<int, int> GridCoordinates(const Point &p) const {
    float dx = p.x() - bbox_.minx();
    float dy = p.y() - bbox_.miny();
    return { int(dx / cell_width_), int(dy / cell_height_) };
  }


  BoundingBox CellBoundingBox(int i, int j) const {
    return BoundingBox(
        bbox_.minx() + i * cell_width_,
        bbox_.miny() + j * cell_height_,
        bbox_.minx() + (i + 1) * cell_width_,
        bbox_.miny() + (j + 1) * cell_height_);
  }


  Point CellCenter(int i, int j) const {
    return {
      bbox_.minx() + (i + 0.5) * cell_width_,
      bbox_.miny() + (j + 0.5) * cell_height_
    };
  }


  const std::vector<key_t> &ItemsInCell(int i, int j) const {
    return items_[i + j * num_cols_];
  }

  std::vector<key_t> &ItemsInCell(int i, int j) {
    return items_[i + j * num_cols_];
  }


  bool InteriorLineSegment(const LineSegment &segment, LineSegment &interior) {
    const Point& a = segment.a();
    const Point& b = segment.b();

    if (a == b) {
      if (bbox_.Contains(a)) {
        interior = LineSegment(a, b);
        return true;
      } else {
        return false;
      }
    }

    auto intersects = BoundingBoxLineSegmentIntersections(bbox_, LineSegment(a, b));
    std::vector<Point> points;
    for (const auto &i : intersects) points.push_back(i.point);

    if (bbox_.Contains(a)) points.push_back(a);
    if (bbox_.Contains(b)) points.push_back(b);

    float mint = 1, maxt = 0;
    Point minp, maxp;
    for (const auto &p : points) {
      float t = Unlerp(a, b, p);
      if (t < mint) {
        mint = t;
        minp = p;
      }
      if (t > maxt) {
        maxt = t;
        maxp = p;
      }
    }

    if (mint < 1 && maxt > 0) {
      assert(mint <= maxt);
      interior = LineSegment(minp, maxp);
      return true;
    } else {
      return false;
    }
  }


  // Index a line segment into the grid
  void AddLineSegment(const key_t edgeid, const LineSegment& segment) {
    LineSegment interior;

    // Do nothing if segment is completely outside the box
    if (!InteriorLineSegment(segment, interior)) return;

    const Point& start = interior.a();
    const Point& end = interior.b();

    Point current_point = start;
    int i, j;
    std::tie(i, j) = GridCoordinates(current_point);

    // Special case
    if (start == end) {
      ItemsInCell(i, j).push_back(edgeid);
      return;
    }

    // Walk along start,end
    while (Unlerp(start, end, current_point) < 1.0) {
      ItemsInCell(i, j).push_back(edgeid);

      const auto& intersects = CellLineSegmentIntersections(i, j, LineSegment(current_point, end));

      float bestd = end.DistanceSquared(CellCenter(i, j));
      BoundingBoxIntersection bestp;
      for (const auto &intersect : intersects) {
        float d = end.DistanceSquared(CellCenter(i + intersect.dx, j + intersect.dy));
        if (d < bestd) {
          bestd = d;
          bestp = intersect;
        }
      }
      if (bestd < end.DistanceSquared(CellCenter(i, j))) {
        current_point = bestp.point;
        i += bestp.dx;
        j += bestp.dy;
      } else {
        break;
      }
    }
  }

  // Query all edges that intersects with the range
  std::unordered_set<key_t> Query(const BoundingBox& range) const {
    std::unordered_set<key_t> results;

    int mini, minj, maxi, maxj;
    std::tie(mini, minj) = GridCoordinates(range.minpt());
    std::tie(maxi, maxj) = GridCoordinates(range.maxpt());

    mini = std::max(0, std::min(mini, num_cols_ - 1));
    maxi = std::max(0, std::min(maxi, num_cols_ - 1));
    minj = std::max(0, std::min(minj, num_rows_ - 1));
    maxj = std::max(0, std::min(maxj, num_rows_ - 1));

    for (int i = mini; i <= maxi; ++i) {
      for (int j = minj; j <= maxj; ++j) {
        const auto& items = ItemsInCell(i, j);
        results.insert(items.begin(), items.end());
      }
    }

    return results;
  }


  // Return t such that p = a + t * (b - a)
  float Unlerp(const Point &a, const Point &b, const Point &p) const {
    if (std::abs(b.x() - a.x()) > std::abs(b.y() - a.y())) {
      return (p.x() - a.x()) / (b.x() - a.x());
    } else {
      return (p.y() - a.y()) / (b.y() - a.y());
    }
  }


  std::vector<BoundingBoxIntersection>
  CellLineSegmentIntersections(int i, int j, const LineSegment &segment) const {
    BoundingBox box = CellBoundingBox(i, j);
    return BoundingBoxLineSegmentIntersections(box, segment);
  }


  std::vector<BoundingBoxIntersection>
  BoundingBoxLineSegmentIntersections(const BoundingBox &box, const LineSegment &segment) const {
    std::vector<BoundingBoxIntersection> intersects;

    LineSegment e1({box.minx(), box.miny()}, {box.maxx(), box.miny()});
    LineSegment e2({box.maxx(), box.miny()}, {box.maxx(), box.maxy()});
    LineSegment e3({box.maxx(), box.maxy()}, {box.minx(), box.maxy()});
    LineSegment e4({box.minx(), box.maxy()}, {box.minx(), box.miny()});

    Point intersect;
    if (segment.Intersect(e1, intersect))
      intersects.push_back(BoundingBoxIntersection({intersect, 0, -1}));
    if (segment.Intersect(e2, intersect))
      intersects.push_back(BoundingBoxIntersection({intersect,  1,  0}));
    if (segment.Intersect(e3, intersect))
      intersects.push_back(BoundingBoxIntersection({intersect,  0,  1}));
    if (segment.Intersect(e4, intersect))
      intersects.push_back(BoundingBoxIntersection({intersect, -1,  0}));

    return intersects;
  }


 private:
  BoundingBox bbox_;
  float cell_width_;
  float cell_height_;
  int num_rows_;
  int num_cols_;
  std::vector<std::vector<key_t> > items_;
};


}

}


#endif // MMP_GRID_RANGE_QUERY_H_
