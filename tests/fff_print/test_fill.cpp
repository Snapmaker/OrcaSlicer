#include <catch2/catch.hpp>

#include <algorithm>
#include <numeric>
#include <sstream>
#include <string>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/Fill.hpp"
#include "libslic3r/Fill/FillGyroid.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

using namespace Slic3r;

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle = 0, double density = 1.0);

#if 0
TEST_CASE("Fill: adjusted solid distance") {
    int surface_width = 250;
    int distance = Slic3r::Flow::solid_spacing(surface_width, 47);
    REQUIRE(distance == Approx(50));
    REQUIRE(surface_width % distance == 0);
}
#endif

TEST_CASE("Fill: Pattern Path Length", "[Fill]") {
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
    filler->angle = float(-(PI)/2.0);
	FillParams fill_params;
	filler->spacing = 5;
	fill_params.dont_adjust = true;
	//fill_params.endpoints_overlap = false;
	fill_params.density = float(filler->spacing / 50.0);

    auto test = [&filler, &fill_params] (const ExPolygon& poly) -> Slic3r::Polylines {
        Slic3r::Surface surface(stTop, poly);
        return filler->fill_surface(&surface, fill_params);
    };

    SECTION("Square") {
        Slic3r::Points test_set;
        test_set.reserve(4);
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        for (size_t i = 0; i < 4; ++i) {
            std::transform(points.cbegin()+i, points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } ); 
            std::transform(points.cbegin(), points.cbegin()+i, std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
            Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
            REQUIRE(paths.size() == 1); // one continuous path

            // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
            // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
            // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
            REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length

            test_set.clear();
        }
    }
    SECTION("Diamond with endpoints on grid") {
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(150,50), Vec2d(100,100), Vec2d(0,100), Vec2d(-50,50)};
        Slic3r::Points test_set;
        test_set.reserve(6);
        std::transform(points.cbegin(), points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
        REQUIRE(paths.size() == 1); // one continuous path
    }

    SECTION("Square with hole") {
        std::vector<Vec2d> square {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        std::vector<Vec2d> hole {Vec2d(25,25), Vec2d(75,25), Vec2d(75,75), Vec2d(25,75) };
        std::reverse(hole.begin(), hole.end());

        Slic3r::Points test_hole;
        Slic3r::Points test_square;

        std::transform(square.cbegin(), square.cend(), std::back_inserter(test_square), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        std::transform(hole.cbegin(), hole.cend(), std::back_inserter(test_hole), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );

        for (double angle : {-(PI/2.0), -(PI/4.0), -(PI), PI/2.0, PI}) {
            for (double spacing : {25.0, 5.0, 7.5, 8.5}) {
				fill_params.density = float(filler->spacing / spacing);
                filler->angle = float(angle);
                ExPolygon e(test_square, test_hole);
                Slic3r::Polylines paths = test(e);
#if 0
				{
					BoundingBox bbox = get_extents(e);
					SVG svg("c:\\data\\temp\\square_with_holes.svg", bbox);
					svg.draw(e);
					svg.draw(paths);
					svg.Close();
				}
#endif
                REQUIRE((paths.size() >= 1 && paths.size() <= 3));
                // paths don't cross hole
                REQUIRE(diff_pl(paths, offset(e, float(SCALED_EPSILON*10))).size() == 0);
            }
        }
    }
    SECTION("Regression: Missing infill segments in some rare circumstances") {
        filler->angle = float(PI/4.0);
		fill_params.dont_adjust = false;
        filler->spacing = 0.654498;
        //filler->endpoints_overlap = unscale(359974);
		fill_params.density = 1;
        filler->layer_id = 66;
        filler->z = 20.15;

        Slic3r::Points points {Point(25771516,14142125),Point(14142138,25771515),Point(2512749,14142131),Point(14142125,2512749)};
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(points));
        REQUIRE(paths.size() == 1); // one continuous path

        // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
        // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
        // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
        REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length
    }

    SECTION("Rotated Square") {
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(50,0), Point::new_scale(50,50), Point::new_scale(0,50)};
        Slic3r::ExPolygon expolygon(square);
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
		filler->bounding_box = get_extents(expolygon.contour);
        filler->angle = 0;
        
        Surface surface(stTop, expolygon);
        auto flow = Slic3r::Flow(0.69f, 0.4f, 0.50f);

		FillParams fill_params;
		fill_params.density = 1.0;
		filler->spacing = flow.spacing();

        for (auto angle : { 0.0, 45.0}) {
            surface.expolygon.rotate(angle, Point(0,0));
            Polylines paths = filler->fill_surface(&surface, fill_params);
            REQUIRE(paths.size() == 1);
        }
    }

    #if 0   // Disabled temporarily due to precission issues on the Mac VM
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(6883102, 9598327.01296997),
            Point::new_scale(6883102, 20327272.01297),
            Point::new_scale(3116896, 20327272.01297),
            Point::new_scale(3116896, 9598327.01296997) 
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        for (size_t i = 0; i <= 20; ++i)
        {
            expolygon.scale(1.05);
            REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        }
    }
    #endif

    SECTION("Solid surface fill") {
        Slic3r::Points points {
                Slic3r::Point(59515297,5422499),Slic3r::Point(59531249,5578697),Slic3r::Point(59695801,6123186),
                Slic3r::Point(59965713,6630228),Slic3r::Point(60328214,7070685),Slic3r::Point(60773285,7434379),
                Slic3r::Point(61274561,7702115),Slic3r::Point(61819378,7866770),Slic3r::Point(62390306,7924789),
                Slic3r::Point(62958700,7866744),Slic3r::Point(63503012,7702244),Slic3r::Point(64007365,7434357),
                Slic3r::Point(64449960,7070398),Slic3r::Point(64809327,6634999),Slic3r::Point(65082143,6123325),
                Slic3r::Point(65245005,5584454),Slic3r::Point(65266967,5422499),Slic3r::Point(66267307,5422499),
                Slic3r::Point(66269190,8310081),Slic3r::Point(66275379,17810072),Slic3r::Point(66277259,20697500),
                Slic3r::Point(65267237,20697500),Slic3r::Point(65245004,20533538),Slic3r::Point(65082082,19994444),
                Slic3r::Point(64811462,19488579),Slic3r::Point(64450624,19048208),Slic3r::Point(64012101,18686514),
                Slic3r::Point(63503122,18415781),Slic3r::Point(62959151,18251378),Slic3r::Point(62453416,18198442),
                Slic3r::Point(62390147,18197355),Slic3r::Point(62200087,18200576),Slic3r::Point(61813519,18252990),
                Slic3r::Point(61274433,18415918),Slic3r::Point(60768598,18686517),Slic3r::Point(60327567,19047892),
                Slic3r::Point(59963609,19493297),Slic3r::Point(59695865,19994587),Slic3r::Point(59531222,20539379),
                Slic3r::Point(59515153,20697500),Slic3r::Point(58502480,20697500),Slic3r::Point(58502480,5422499)
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55, PI/2.0) == true);
    }
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(0,0),Point::new_scale(98,0),Point::new_scale(98,10), Point::new_scale(0,10)
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.5, 45.0, 0.99) == true);
    }
}

/*
{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([4,0], [10,0], [15,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([15,0], [10,0], [4,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

for my $pattern (qw(rectilinear honeycomb hilbertcurve concentric)) {
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('fill_pattern', $pattern);
    $config->set('external_fill_pattern', $pattern);
    $config->set('perimeters', 1);
    $config->set('skirts', 0);
    $config->set('fill_density', 20);
    $config->set('layer_height', 0.05);
    $config->set('perimeter_extruder', 1);
    $config->set('infill_extruder', 2);
    my $print = Slic3r::Test::init_print('20mm_cube', config => $config, scale => 2);
    ok my $gcode = Slic3r::Test::gcode($print), "successful $pattern infill generation";
    my $tool = undef;
    my @perimeter_points = my @infill_points = ();
    Slic3r::GCode::Reader->new->parse($gcode, sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->perimeter_extruder-1) {
            push @perimeter_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            } elsif ($tool == $config->infill_extruder-1) {
            push @infill_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            }
            }
            });
    my $convex_hull = convex_hull(\@perimeter_points);
    ok !(defined first { !$convex_hull->contains_point($_) } @infill_points), "infill does not exceed perimeters ($pattern)";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('infill_only_where_needed', 1);
    $config->set('bottom_solid_layers', 0);
    $config->set('infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.5);
    $config->set('fill_density', 40);
    $config->set('cooling', 0);                 # for preventing speeds from being altered
        $config->set('first_layer_speed', '100%');  # for preventing speeds from being altered

        my $test = sub {
            my $print = Slic3r::Test::init_print('pyramid', config => $config);

            my $tool = undef;
            my @infill_extrusions = ();  # array of polylines
                Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
                        my ($self, $cmd, $args, $info) = @_;

                        if ($cmd =~ /^T(\d+)/) {
                        $tool = $1;
                        } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
                        if ($tool == $config->infill_extruder-1) {
                        push @infill_extrusions, Slic3r::Line->new_scale(
                                [ $self->X, $self->Y ],
                                [ $info->{new_X}, $info->{new_Y} ],
                                );
                        }
                        }
                        });
            return 0 if !@infill_extrusions;  # prevent calling convex_hull() with no points

                my $convex_hull = convex_hull([ map $_->pp, map @$_, @infill_extrusions ]);
            return unscale unscale sum(map $_->area, @{offset([$convex_hull], scale(+$config->infill_extrusion_width/2))});
        };

    my $tolerance = 5;  # mm^2

        $config->set('solid_infill_below_area', 0);
    ok $test->() < $tolerance,
       'no infill is generated when using infill_only_where_needed on a pyramid';

    $config->set('solid_infill_below_area', 70);
    ok abs($test->() - $config->solid_infill_below_area) < $tolerance,
       'infill is only generated under the forced solid shells';
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 1);
    $config->set('fill_density', 0);
    $config->set('top_solid_layers', 0);
    $config->set('bottom_solid_layers', 0);
    $config->set('solid_infill_below_area', 20000000);
    $config->set('solid_infill_every_layers', 2);
    $config->set('perimeter_speed', 99);
    $config->set('external_perimeter_speed', 99);
    $config->set('cooling', 0);
    $config->set('first_layer_speed', '100%');

    my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %layers_with_extrusion = ();
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd eq 'G1' && $info->{dist_XY} > 0 && $info->{extruding}) {
            if (($args->{F} // $self->F) != $config->perimeter_speed*60) {
            $layers_with_extrusion{$self->Z} = ($args->{F} // $self->F);
            }
            }
            });

    ok !%layers_with_extrusion,
       "solid_infill_below_area and solid_infill_every_layers are ignored when fill_density is 0";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 3);
    $config->set('fill_density', 0);
    $config->set('layer_height', 0.2);
    $config->set('first_layer_height', 0.2);
    $config->set('nozzle_diameter', [0.35]);
    $config->set('infill_extruder', 2);
    $config->set('solid_infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.52);
    $config->set('solid_infill_extrusion_width', 0.52);
    $config->set('first_layer_extrusion_width', 0);

    my $print = Slic3r::Test::init_print('A', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->infill_extruder-1) {
            my $z = 1 * $self->Z;
            $infill{$z} ||= [];
            push @{$infill{$z}}, Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            }
            }
            });
    my $grow_d = scale($config->infill_extrusion_width)/2;
    my $layer0_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.2} } ]);
    my $layer1_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.4} } ]);
    my $diff = diff($layer0_infill, $layer1_infill);
    $diff = offset2_ex($diff, -$grow_d, +$grow_d);
    $diff = [ grep { $_->area > 2*(($grow_d*2)**2) } @$diff ];
    is scalar(@$diff), 0, 'no missing parts in solid shell when fill_density is 0';
}

{
    # GH: #2697
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('perimeter_extrusion_width', 0.72);
    $config->set('top_infill_extrusion_width', 0.1);
    $config->set('infill_extruder', 2);         # in order to distinguish infill
        $config->set('solid_infill_extruder', 2);   # in order to distinguish infill

        my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my %other  = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            my $z = 1 * $self->Z;
            my $line = Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            if ($tool == $config->infill_extruder-1) {
            $infill{$z} //= [];
            push @{$infill{$z}}, $line;
            } else {
            $other{$z} //= [];
            push @{$other{$z}}, $line;
            }
            }
            });
    my $top_z = max(keys %infill);
    my $top_infill_grow_d = scale($config->top_infill_extrusion_width)/2;
    my $top_infill = union([ map @{$_->grow($top_infill_grow_d)}, @{ $infill{$top_z} } ]);
    my $perimeters_grow_d = scale($config->perimeter_extrusion_width)/2;
    my $perimeters = union([ map @{$_->grow($perimeters_grow_d)}, @{ $other{$top_z} } ]);
    my $covered = union_ex([ @$top_infill, @$perimeters ]);
    my @holes = map @{$_->holes}, @$covered;
    ok sum(map unscale unscale $_->area*-1, @holes) < 1, 'no gaps between top solid infill and perimeters';
}
*/

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle, double density)
{
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
	filler->bounding_box = get_extents(expolygon.contour);
    filler->angle = float(angle);

	Flow flow(float(flow_spacing), 0.4f, float(flow_spacing));
	filler->spacing = flow.spacing();

	FillParams fill_params;
	fill_params.density = float(density);
	fill_params.dont_adjust = false;

	Surface surface(stBottom, expolygon);
	Slic3r::Polylines paths = filler->fill_surface(&surface, fill_params);

    // check whether any part was left uncovered
    Polygons grown_paths;
    grown_paths.reserve(paths.size());

    // figure out what is actually going on here re: data types
    float line_offset = float(scale_(filler->spacing / 2.0 + EPSILON));
    std::for_each(paths.begin(), paths.end(), [line_offset, &grown_paths] (const Slic3r::Polyline& p) {
        polygons_append(grown_paths, offset(p, line_offset));
    });

	// Shrink the initial expolygon a bit, this simulates the infill / perimeter overlap that we usually apply.
    ExPolygons uncovered = diff_ex(offset(expolygon, - float(0.2 * scale_(flow_spacing))), grown_paths, ApplySafetyOffset::Yes);

    // ignore very small dots
    const double scaled_flow_spacing = std::pow(scale_(flow_spacing), 2);
    uncovered.erase(std::remove_if(uncovered.begin(), uncovered.end(), [scaled_flow_spacing](const ExPolygon& poly) { return poly.area() < scaled_flow_spacing; }), uncovered.end());

#if 0
	if (! uncovered.empty()) {
		BoundingBox bbox = get_extents(expolygon.contour);
		bbox.merge(get_extents(uncovered));
		bbox.merge(get_extents(grown_paths));
		SVG svg("c:\\data\\temp\\test_if_solid_surface_filled.svg", bbox);
		svg.draw(expolygon);
		svg.draw(uncovered, "red");
		svg.Close();
	}
#endif

    return uncovered.empty(); // solid surface is fully filled
}

TEST_CASE("Sparse plane-path anchors match the printed infill", "[Fill][InternalBridge][Regression]")
{
    // Orca: Compare generated anchors with actual extrusion across plane-path patterns,
    // multiline and rotations; an origin shift must not pass as valid support.
    // Ultra has no sparse_infill_smooth_factor / separated_infills, so those
    // dimensions from OrcaSlicer#15206 are omitted.
    const std::string pattern = GENERATE("hilbertcurve", "octagramspiral", "archimedeanchords");
    const int multiline = GENERATE(1, 2);
    const bool rotated = GENERATE(false, true);
    CAPTURE(pattern, multiline, rotated);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "15%"},
                                   {"fill_multiline", multiline},
                                   {"infill_direction", 45},
                                   {"sparse_infill_rotate_template", rotated ? "0,25,50" : ""},
                                   {"align_infill_direction_to_model", rotated},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"resolution", 0.012}});
    Print print;
    Model model;
    Slic3r::Test::init_print({make_cube(30, 24, 1)}, print, model, config, false);
    if (rotated) {
        model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(23.)));
        print.apply(model, config);
    }
    print.process();

    const Layer &layer = *print.objects().front()->get_layer(4);
    Polylines printed;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == erInternalInfill)
                entity->collect_polylines(printed);
    REQUIRE_FALSE(printed.empty());
    const AABBTreeLines::LinesDistancer<Line> printed_tree(to_lines(printed));

    // Orca: Exclude perimeter connections: anchoring and extrusion can trim those differently.
    const Polylines anchors = intersection_pl(layer.generate_sparse_infill_polylines_for_anchoring(nullptr, nullptr, nullptr),
                                              shrink(to_polygons(layer.lslices), scale_(3.)));
    REQUIRE_FALSE(anchors.empty());
    double max_distance = 0.;
    for (const Polyline &path : anchors)
        for (const Point &point : path.equally_spaced_points(scale_(0.25)))
            max_distance = std::max(max_distance, printed_tree.distance_from_lines<false>(point));
    // Orca: Allow only the configured simplification tolerance; infill-scale offsets
    // would hide anchors that no longer coincide with printed lines.
    CHECK(unscale<double>(max_distance) <= config.opt_float("resolution"));
}

TEST_CASE("Locked Zag bands fall back for patterns that need per-object state", "[Fill][LockedZag]")
{
    // adaptivecubic, supportcubic and lightning are left out of the locked_sk*_infill_pattern menus
    // because their fillers need an octree / generator that is only built when a region's own sparse
    // pattern asks for one. A stored value can still carry them (a Bambu preset lists them; the GUI
    // combo stored its row index as the value before the key mapping), so slicing must not
    // dereference the missing state - the band keeps the Locked Zag filler's own pattern instead.
    const std::string pattern = GENERATE("adaptivecubic", "supportcubic", "lightning");
    const bool        in_skin = GENERATE(false, true);
    CAPTURE(pattern, in_skin);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", "lockedzag"},
                                   {"sparse_infill_density", "20%"},
                                   {"locked_skin_infill_pattern", in_skin ? pattern : std::string("default")},
                                   {"locked_skeleton_infill_pattern", in_skin ? std::string("default") : pattern},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2}});
    Print print;
    Model model;
    Slic3r::Test::init_print({make_cube(30, 30, 6)}, print, model, config, false);
    print.process();

    const Layer &layer = *print.objects().front()->get_layer(10);
    Polylines printed;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == erInternalInfill)
                entity->collect_polylines(printed);
    CHECK_FALSE(printed.empty());
}

// Slices a 30x30x6 mm cube at 0.2 mm layers (layers 0..29) with the given surface settings.
static void process_surface_density_cube(Print &print, Model &model, std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"top_shell_layers", 4},
                                   {"bottom_shell_layers", 3},
                                   {"sparse_infill_density", "15%"}});
    config.set_deserialize_strict(items);
    Slic3r::Test::init_print({make_cube(30, 30, 6)}, print, model, config, false);
    print.process();
}

static Polylines fill_polylines(const Layer &layer, ExtrusionRole role)
{
    Polylines out;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == role)
                entity->collect_polylines(out);
    return out;
}

static double fill_length_mm(const Polylines &polylines)
{
    return std::accumulate(polylines.begin(), polylines.end(), 0.,
                           [](double acc, const Polyline &pl) { return acc + unscale<double>(pl.length()); });
}

TEST_CASE("Top and bottom surface density", "[Fill][SurfaceDensity]")
{
    Print print;
    Model model;

    SECTION("0% top surface density prints only the walls on the top layer") {
        // Fillers divide their line spacing by the density, so a 0% top surface must be dropped
        // before it reaches them rather than filled with an infinite spacing.
        process_surface_density_cube(print, model, {{"top_surface_density", "0%"}});
        const Layer &top = *print.objects().front()->get_layer(29);
        CHECK(fill_polylines(top, erTopSolidInfill).empty());
        CHECK_FALSE(top.regions().front()->perimeters.entities.empty());
    }

    SECTION("A lower top surface density spaces the top lines apart") {
        process_surface_density_cube(print, model, {{"top_surface_density", "100%"}});
        const double full = fill_length_mm(fill_polylines(*print.objects().front()->get_layer(29), erTopSolidInfill));
        Print print_half;
        Model model_half;
        process_surface_density_cube(print_half, model_half, {{"top_surface_density", "50%"}});
        const double half = fill_length_mm(fill_polylines(*print_half.objects().front()->get_layer(29), erTopSolidInfill));
        CAPTURE(full, half);
        REQUIRE(full > 0.);
        CHECK(half > 0.3 * full);
        CHECK(half < 0.75 * full);
    }

    SECTION("A lower bottom surface density spaces the bottom lines apart") {
        process_surface_density_cube(print, model, {{"bottom_surface_density", "100%"}});
        const double full = fill_length_mm(fill_polylines(*print.objects().front()->get_layer(0), erBottomSurface));
        Print print_half;
        Model model_half;
        process_surface_density_cube(print_half, model_half, {{"bottom_surface_density", "50%"}});
        const double half = fill_length_mm(fill_polylines(*print_half.objects().front()->get_layer(0), erBottomSurface));
        CAPTURE(full, half);
        REQUIRE(full > 0.);
        CHECK(half > 0.3 * full);
        CHECK(half < 0.75 * full);
    }
}

TEST_CASE("Undertop surface pattern fills the solid layer under a sparse top", "[Fill][SurfaceDensity]")
{
    // Share of the printed length running parallel to the cube's X or Y edges. Concentric rings on a
    // square are axis-aligned; the default monotonic solid infill runs at 45 degrees.
    auto axis_aligned_share = [](const Polylines &polylines) {
        double aligned = 0., total = 0.;
        for (const Polyline &pl : polylines)
            for (const Line &line : pl.lines()) {
                const Vec2d  d   = (line.b - line.a).cast<double>();
                const double len = d.norm();
                total += len;
                if (std::min(std::abs(d.x()), std::abs(d.y())) < 0.1 * len)
                    aligned += len;
            }
        return total > 0. ? aligned / total : 0.;
    };

    const std::string undertop = GENERATE("default", "concentric");
    const std::string density  = GENERATE("50%", "100%");
    CAPTURE(undertop, density);

    Print print;
    Model model;
    process_surface_density_cube(print, model, {{"top_surface_density", density}, {"undertop_surface_pattern", undertop}});
    // Layer 28 is the solid layer directly under the top skin on layer 29.
    const Polylines under_top = fill_polylines(*print.objects().front()->get_layer(28), erSolidInfill);
    REQUIRE_FALSE(under_top.empty());
    const double share = axis_aligned_share(under_top);
    CAPTURE(share);
    // The undertop pattern only takes over while the top surface is sparse enough to show it.
    if (undertop == "concentric" && density == "50%")
        CHECK(share > 0.8);
    else
        CHECK(share < 0.5);
}

TEST_CASE("Gyroid infill of an object matches the infill of a larger object with the same center", "[Fill]")
{
    // Orca #16002 parametric half: Edge has no marching-squares / gyroid_optimized
    // branch, so that GENERATE dimension is dropped. DensityAdjust and
    // AABBTreeLines::LinesDistancer both exist on Edge; the test uses them as upstream does.
    const int    multiline = GENERATE(1, 2);
    const float  density   = GENERATE(0.05f, 0.2f);
    const double spacing   = 0.45;
    CAPTURE(multiline, density);

    auto circle = [](double radius) {
        Polygon contour = make_circle_num_segments(scale_(radius), 120);
        contour.translate(Point::new_scale(100., 60.));
        return ExPolygon(std::move(contour));
    };
    const ExPolygon object = circle(20.);
    const ExPolygon larger = circle(30.);
    auto fill = [multiline, density, spacing](const ExPolygon &region, double z) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipGyroid));
        filler->spacing = spacing;
        filler->angle   = float(M_PI / 7.);
        filler->z       = z;

        FillParams params;
        params.density     = density;
        params.multiline   = multiline;
        params.dont_adjust = true;
        Surface surface(stInternal, region);
        return filler->fill_surface(&surface, params);
    };
    // Away from the boundary of the object, where both are clipped and connected the same way.
    const Polygons inner = shrink(to_polygons(object), scale_(1.));
    auto farthest = [&inner](const Polylines &from, const Polylines &to) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(to));
        double distance = 0.;
        for (const Polyline &path : intersection_pl(from, inner))
            for (const Point &point : path.equally_spaced_points(scale_(0.2)))
                distance = std::max(distance, tree.distance_from_lines<false>(point));
        return unscale<double>(distance);
    };

    const double tolerance = 0.01;
    // Half a z period of the waves, through both switches between horizontal and vertical waves.
    const double wave_distance = spacing * multiline / (density * FillGyroid::DensityAdjust);
    for (int step = 0; step <= 8; ++step) {
        const double z = wave_distance * M_PI * step / 8.;
        CAPTURE(z);
        const Polylines paths = fill(object, z);
        REQUIRE_FALSE(paths.empty());
        const Polylines reference = fill(larger, z);
        CHECK(farthest(reference, paths) < tolerance);
        CHECK(farthest(paths, reference) < tolerance);
    }
}

TEST_CASE("Gyroid multiline-1 waves stay pinned and cover the contour edge", "[Fill]")
{
    // The same-center case above shrinks 1 mm inward, so it stays green on main and
    // would miss a global phase shift or a dropped strip at the bbox edge. These
    // pins are world-mm vertices of the phase-preserving generator (multiline 1,
    // density 0.2, spacing 0.45, angle = π/4 so CorrectionAngle cancels).
    // fill_surface insets the 10..50 mm square by 0.5*spacing first (overlap 0),
    // so the filled region is 10.225..49.775. At z=0 the waves run along Y,
    // ~2.90 mm apart; the last kept wave spans x=46.921..48.370 and the next
    // (49.818..51.266) is clipped, so min_right is 1.6305 mm from x=50, not 0.225.
    const double spacing = 0.45;
    const float  density = 0.2f;
    FillParams params;
    params.density           = density;
    params.multiline         = 1;
    params.dont_adjust       = true;
    params.anchor_length     = 0.f;
    params.anchor_length_max = 0.f; // dont_connect: keep wave vertices unjoined

    Polygon square{
        Point::new_scale(10., 10.), Point::new_scale(50., 10.),
        Point::new_scale(50., 50.), Point::new_scale(10., 50.)
    };
    auto fill_at = [&](double z) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipGyroid));
        filler->spacing = spacing;
        filler->angle   = float(M_PI / 4.);
        filler->z       = z;
        Surface surface(stInternal, ExPolygon(square));
        return filler->fill_surface(&surface, params);
    };
    auto pin_ok = [](const Polylines &paths, double x, double y, double tol) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(paths));
        const Point q = Point::new_scale(x, y);
        const double d = unscale<double>(tree.distance_from_lines<false>(q));
        CAPTURE(x, y, d);
        CHECK(d < tol);
    };

    const Polylines paths0 = fill_at(0.);
    REQUIRE_FALSE(paths0.empty());
    const double pin_tol = 0.02;
    // z=0: waves run along Y. A shifted origin moves these by millimetres.
    const double pins_y[][2] = {
        {13.449314, 33.884724},
        {15.211169, 13.606001},
        {24.469603, 30.263524},
        {30.263524, 30.263524},
        {41.851366, 30.263524},
        {46.921046, 31.712004},
    };
    for (const auto &xy : pins_y)
        pin_ok(paths0, xy[0], xy[1], pin_tol);

    // Reach to the original 10..50 mm sides after the 0.5*spacing inset + clip.
    // Pin the four values (tol 0.02): a dropped strip or a looser 1 mm gate on
    // the sparse +X side would miss the real 1.6305 mm right reach.
    double min_left = 1e9, min_right = 1e9, min_bottom = 1e9, min_top = 1e9;
    for (const Polyline &pl : paths0)
        for (const Point &p : pl.points) {
            const double x = unscale<double>(p.x());
            const double y = unscale<double>(p.y());
            min_left   = std::min(min_left,   std::abs(x - 10.));
            min_right  = std::min(min_right,  std::abs(x - 50.));
            min_bottom = std::min(min_bottom, std::abs(y - 10.));
            min_top    = std::min(min_top,    std::abs(y - 50.));
        }
    CAPTURE(min_left, min_right, min_bottom, min_top);
    CHECK(std::abs(min_left   - 0.225)  < 0.02);
    CHECK(std::abs(min_right  - 1.6305) < 0.02);
    CHECK(std::abs(min_bottom - 0.225)  < 0.02);
    CHECK(std::abs(min_top    - 0.225)  < 0.02);

    // Second pin set: z such that the pattern angle is π/2, so waves run along X.
    // At z=0 some X shifts look identical; these Y-separated vertices would not.
    const double z_along_x = (M_PI / 2.) * spacing / (density * FillGyroid::DensityAdjust);
    const Polylines pathsX = fill_at(z_along_x);
    REQUIRE_FALSE(pathsX.empty());
    const double pins_x[][2] = {
        {30.263524, 15.054482},
        {30.263524, 29.539284},
        {13.606001, 29.382597},
        {30.263524, 41.127126},
    };
    for (const auto &xy : pins_x)
        pin_ok(pathsX, xy[0], xy[1], pin_tol);
}
