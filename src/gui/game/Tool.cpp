#include "Tool.h"

#include "graphics/Graphics.h"
#include "gui/game/Brush.h"

#include "simulation/Simulation.h"
#include "simulation/SimulationData.h"
#include "simulation/ElementClasses.h"

std::unique_ptr<VideoBuffer> Tool::GetTexture(Vec2<int> size)
{
	return textureGen ? textureGen(ToolID, size) : nullptr;
}

void Tool::Click(Simulation * sim, Brush const &brush, ui::Point position) { }
void Tool::Draw(Simulation * sim, Brush const &brush, ui::Point position) {
	sim->ToolBrush(position.X, position.Y, ToolID, brush, Strength);
}
void Tool::DrawLine(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2, bool dragging) {
	sim->ToolLine(position1.X, position1.Y, position2.X, position2.Y, ToolID, brush, Strength);
}
void Tool::DrawRect(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2) {
	sim->ToolBox(position1.X, position1.Y, position2.X, position2.Y, ToolID, Strength);
}
void Tool::DrawFill(Simulation * sim, Brush const &brush, ui::Point position) {}


void ElementTool::Draw(Simulation * sim, Brush const &brush, ui::Point position){
	sim->CreateParts(position.X, position.Y, ToolID, brush);
}
void ElementTool::DrawLine(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2, bool dragging) {
	sim->CreateLine(position1.X, position1.Y, position2.X, position2.Y, ToolID, brush);
}
void ElementTool::DrawRect(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2) {
	sim->CreateBox(position1.X, position1.Y, position2.X, position2.Y, ToolID);
}
void ElementTool::DrawFill(Simulation * sim, Brush const &brush, ui::Point position) {
	sim->FloodParts(position.X, position.Y, ToolID, -1);
}


void WallTool::Draw(Simulation * sim, Brush const &brush, ui::Point position) {
	sim->CreateWalls(position.X, position.Y, 1, 1, ToolID, &brush);
}
void WallTool::DrawLine(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2, bool dragging) {
	int wallX = position1.X/CELL;
	int wallY = position1.Y/CELL;
	if(dragging == false && ToolID == WL_FAN && sim->bmap[wallY][wallX]==WL_FAN)
	{
		float newFanVelX = (position2.X-position1.X)*0.005f;
		newFanVelX *= Strength;
		float newFanVelY = (position2.Y-position1.Y)*0.005f;
		newFanVelY *= Strength;
		sim->FloodWalls(position1.X, position1.Y, WL_FLOODHELPER, WL_FAN);
		for (int j = 0; j < YCELLS; j++)
			for (int i = 0; i < XCELLS; i++)
				if (sim->bmap[j][i] == WL_FLOODHELPER)
				{
					sim->fvx[j][i] = newFanVelX;
					sim->fvy[j][i] = newFanVelY;
					sim->bmap[j][i] = WL_FAN;
				}
	}
	else
	{
		sim->CreateWallLine(position1.X, position1.Y, position2.X, position2.Y, 1, 1, ToolID, &brush);
	}
}
void WallTool::DrawRect(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2) {
	sim->CreateWallBox(position1.X, position1.Y, position2.X, position2.Y, ToolID);
}
void WallTool::DrawFill(Simulation * sim, Brush const &brush, ui::Point position) {
	if (ToolID != WL_STREAM)
		sim->FloodWalls(position.X, position.Y, ToolID, -1);
}

void WindTool::DrawLine(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2, bool dragging)
{
	float strength = dragging?0.01f:0.002f;
	strength *= this->Strength;

	for (ui::Point off : brush)
	{
		ui::Point coords = position1 + off;
		if (coords.X >= 0 && coords.Y >= 0 && coords.X < XRES && coords.Y < YRES)
		{
			sim->vx[coords.Y / CELL][coords.X / CELL] += (position2 - position1).X * strength;
			sim->vy[coords.Y / CELL][coords.X / CELL] += (position2 - position1).Y * strength;
		}
	}
}


void Element_LIGH_Tool::DrawLine(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2, bool dragging)
{
	if (dragging)
		sim->CreateParts(position1.X, position1.Y, brush.GetRadius().X, brush.GetRadius().Y, PT_LIGH);
}


void Element_TESC_Tool::DrawRect(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2) {
	int radiusInfo = brush.GetRadius().X*4+brush.GetRadius().Y*4+7;
	sim->CreateBox(position1.X, position1.Y, position2.X, position2.Y, ToolID | PMAPID(radiusInfo));
}
void Element_TESC_Tool::DrawFill(Simulation * sim, Brush const &brush, ui::Point position) {
	int radiusInfo = brush.GetRadius().X*4+brush.GetRadius().Y*4+7;
	sim->FloodParts(position.X, position.Y, ToolID | PMAPID(radiusInfo), -1);
}


void PlopTool::Click(Simulation * sim, Brush const &brush, ui::Point position)
{
	sim->create_part(-2, position.X, position.Y, TYP(ToolID), ID(ToolID));
}
