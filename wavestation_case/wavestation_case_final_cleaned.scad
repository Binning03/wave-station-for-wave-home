// WaveStation OpenSCAD final model
// Size: 110 x 75 x 90 mm approx.
// Lid fit: 0.65 mm total clearance in X/Y, 0.30 mm friction bumps on all sides.
// Units: mm
// Orientation: X left-right, Y front-back, Z vertical
// Front Y=0 faces bed/room, Back Y=D faces wall/headboard
// X=0 is left/corner, X=W is right/window/desk

$fn = 64;

W = 110;
D = 75;
BASE_H = 82;
LID_H = 8;
WALL = 2.4;
BOTTOM = 3;
LID_TOP = 3.2;

// Lid fit tuning
// CLEAR_X/Y are total clearances, not per-side clearances.
// With 0.65 mm total clearance, each side has about 0.325 mm gap.
// Friction bumps are 0.30 mm thick on all four sides.
CLEAR_X = 0.65;
CLEAR_Y = 0.65;

// ===== 2D helpers =====
module slot2d(w,h,r=1.2) {
    offset(r=r) offset(delta=-r) square([w,h], center=true);
}

// Capsule: rectangle with semicircles on left/right sides.
// w = total width, h = total height. Must satisfy w >= h.
module capsule2d(w,h) {
    hull() {
        translate([-w/2 + h/2, 0]) circle(d=h);
        translate([ w/2 - h/2, 0]) circle(d=h);
    }
}

// ===== Front wall cutters, Y=0 =====
module cut_front_slot(x,z,w,h,r=1.5) {
    translate([x, -1, z]) rotate([-90,0,0])
        linear_extrude(height=WALL+3)
            slot2d(w,h,r);
}

module cut_front_circle(x,z,d) {
    translate([x, -1, z]) rotate([-90,0,0])
        cylinder(h=WALL+3, d=d);
}

module cut_front_capsule(x,z,w,h) {
    translate([x, -1, z]) rotate([-90,0,0])
        linear_extrude(height=WALL+3)
            capsule2d(w,h);
}

// ===== Side wall cutters =====
// Left side X=0. len_y = front-back length, height_z = vertical height.
module cut_left_slot(y,z,len_y,height_z,r=1.5) {
    translate([-1,0,0])
        hull() {
            translate([0, y-len_y/2+height_z/2, z]) rotate([0,90,0]) cylinder(h=WALL+4, d=height_z);
            translate([0, y+len_y/2-height_z/2, z]) rotate([0,90,0]) cylinder(h=WALL+4, d=height_z);
        }
}

// Right side X=W. len_y = front-back length, height_z = vertical height.
module cut_right_slot(y,z,len_y,height_z,r=1.5) {
    translate([W-WALL-1,0,0])
        hull() {
            translate([0, y-len_y/2+height_z/2, z]) rotate([0,90,0]) cylinder(h=WALL+5, d=height_z);
            translate([0, y+len_y/2-height_z/2, z]) rotate([0,90,0]) cylinder(h=WALL+5, d=height_z);
        }
}

// Right side rectangular window cutter: sharp rectangular opening on Y-Z plane.
// len_y = front-back horizontal length, height_z = vertical height.
module cut_right_rect(y,z,len_y,height_z) {
    translate([W-WALL-1, y-len_y/2, z-height_z/2])
        cube([WALL+5, len_y, height_z]);
}

// ===== Top/lid cutters =====
module cut_top_circle(x,y,d) {
    translate([x,y,LID_H-LID_TOP-1])
        cylinder(h=LID_TOP+3, d=d);
}

module cut_top_slot(x,y,w,h,r=1.0) {
    translate([x,y,LID_H-LID_TOP-1])
        linear_extrude(height=LID_TOP+3)
            slot2d(w,h,r);
}


// ===== Logo / front decoration helpers =====
module line2d(p1, p2, w=2) {
    hull() {
        translate(p1) circle(d=w);
        translate(p2) circle(d=w);
    }
}

module polyline2d(points, w=2) {
    for (i=[0:len(points)-2]) line2d(points[i], points[i+1], w);
}

module arc2d(cx, cy, r, a1, a2, w=2, steps=12) {
    pts = [for (i=[0:steps]) [cx + r*cos(a1 + (a2-a1)*i/steps), cy + r*sin(a1 + (a2-a1)*i/steps)]];
    polyline2d(pts, w);
}

// Simplified WaveStation logo modeled directly in OpenSCAD.
// No external SVG file is required.
module wavestation_logo_2d() {
    union() {
        // house outline
        polyline2d([[-19,-4],[-19,8],[-7,20],[0,25],[7,20],[19,8],[19,-4]], 3.2);

        // 2x2 window
        for (xx=[-3.8,3.8]) for (yy=[3.5,10.5])
            translate([xx,yy]) square([5,5], center=true);

        // wave stroke under the house
        polyline2d([[-24,-10],[-16,-7],[-8,-5],[0,-6],[9,-9],[18,-11],[26,-8]], 4.2);
        polyline2d([[-24,-15],[-14,-12],[-4,-10],[7,-12],[18,-15],[27,-12]], 2.4);

        // wireless signal arcs
        arc2d(18,17,7,20,80,1.9,10);
        arc2d(18,17,12,20,80,1.9,10);
    }
}

module base() {
    difference() {
        union() {
            // Flat bottom + four walls, open top.
            cube([W,D,BOTTOM]);
            cube([W,WALL,BASE_H]);
            translate([0,D-WALL,0]) cube([W,WALL,BASE_H]);
            cube([WALL,D,BASE_H]);
            translate([W-WALL,0,0]) cube([WALL,D,BASE_H]);

            // Raised text on the upper front face.
            translate([W/2, 0.18, 60])
                rotate([90,0,0])
                    linear_extrude(height=0.55)
                        text("Wave Station", size=10, halign="center", valign="center", font="Liberation Sans:style=Bold");
        }

        // LEFT: ESP32 USB-C power opening.
        // Range from back face: 23.5~40.5 mm.
        // Range from bottom:    12~23 mm.
        USB_Y = D - 32;
        USB_Z = 17.5;
        USB_LEN = 17;
        USB_H = 11;
        cut_left_slot(USB_Y, USB_Z, USB_LEN, USB_H, 1.6);

        // RIGHT: temperature/humidity sensor opening.
        // Plain rectangular window, not rounded/capsule.
        DHT_Y = D - 21;
        DHT_Z = 30;      // aligned with front speaker center height
        DHT_WIN_LEN = 21;
        DHT_WIN_H = 16;
        cut_right_rect(DHT_Y, DHT_Z, DHT_WIN_LEN, DHT_WIN_H);

        // FRONT LEFT: microphone opening, 15 mm round.
        cut_front_circle(24, 23, 15);

        // FRONT CENTER: IR receiver visible part only, 6 x 8 mm.
        cut_front_slot(W/2, 37, 6, 8, 1.0);

        // FRONT RIGHT: speaker opening, capsule shape 23 x 14 mm.
        // Shape = rectangle with semicircles on both left/right sides.
        cut_front_capsule(84, 23, 23, 14);

        // BACK: wall/headboard keyholes.
        // BASE_H = 82 mm, keyhole range is roughly Z 31~49 mm.
        KEY_CIRCLE_Z = 46;
        KEY_SLOT_Z = 37;
        for (xx=[32,78]) {
            translate([xx,D-WALL-1,KEY_CIRCLE_Z]) rotate([-90,0,0])
                cylinder(h=WALL+3,d=6.8);
            translate([xx,D-WALL-1,KEY_SLOT_Z]) rotate([-90,0,0])
                linear_extrude(height=WALL+3)
                    slot2d(4.6,12,1.3);
        }
    }
}

module lid() {
    skirtW = W - 2*WALL - CLEAR_X;
    skirtD = D - 2*WALL - CLEAR_Y;
    skirtT = 1.4;
    skirtH = LID_H - 0.6;
    sx = (W-skirtW)/2;
    sy = (D-skirtD)/2;

    difference() {
        union() {
            // Top plate
            translate([0,0,LID_H-LID_TOP]) cube([W,D,LID_TOP]);

            // Raised top logo.
            // Approximate bounds after scaling: X 35~78 mm, Y 36~70 mm.
            // Orientation: upright/readable when facing the front side Y=0.
            translate([W/2, 48, LID_H + 0.05])
                linear_extrude(height=0.6)
                    scale([0.75, 0.75, 1])
                        wavestation_logo_2d();

            // Friction-fit internal skirt
            translate([sx,sy,0]) cube([skirtW,skirtT,skirtH]);
            translate([sx,sy+skirtD-skirtT,0]) cube([skirtW,skirtT,skirtH]);
            translate([sx,sy,0]) cube([skirtT,skirtD,skirtH]);
            translate([sx+skirtW-skirtT,sy,0]) cube([skirtT,skirtD,skirtH]);

            // Friction bumps, 0.30 mm on front/back/left/right.
            translate([W/2-9, sy-0.30, 2]) cube([18,0.30,2.7]);
            translate([W/2-9, sy+skirtD, 2]) cube([18,0.30,2.7]);

            translate([sx-0.30, D/2-8, 2]) cube([0.30,16,2.5]);
            translate([sx+skirtW, D/2-8, 2]) cube([0.30,16,2.5]);
        }

        // TOP LEFT: IR transmitter LED only, 7.2 mm circular hole.
        cut_top_circle(25, 22, 7.2);

        // TOP RIGHT: BH1750 light sensor visible part only, 5.5 x 3.5 mm.
        cut_top_slot(82, 23, 5.5, 3.5, 0.7);
    }
}

// ===== Display/export selector =====
// "assembly" : preview base + lid together
// "base"     : export base only
// "lid"      : export lid only
PART = "assembly";

if (PART == "base") {
    base();
} else if (PART == "lid") {
    lid();
} else {
    base();
    translate([0,0,BASE_H+12]) lid();
}
