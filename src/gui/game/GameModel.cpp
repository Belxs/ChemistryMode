#include "GameModel.h"
#include "BitmapBrush.h"
#include "EllipseBrush.h"
#include "Favorite.h"
#include "Format.h"
#include "GameController.h"
#include "GameModelException.h"
#include "GameView.h"
#include "Menu.h"
#include "Notification.h"
#include "RectangleBrush.h"
#include "TriangleBrush.h"
#include "QuickOptions.h"
#include "lua/CommandInterface.h"
#include "prefs/GlobalPrefs.h"
#include "client/Client.h"
#include "client/GameSave.h"
#include "client/SaveFile.h"
#include "client/SaveInfo.h"
#include "client/http/ExecVoteRequest.h"
#include "common/platform/Platform.h"
#include "common/clipboard/Clipboard.h"
#include "graphics/Renderer.h"
#include "simulation/Air.h"
#include "simulation/GOLString.h"
#include "simulation/gravity/Gravity.h"
#include "simulation/Simulation.h"
#include "simulation/Snapshot.h"
#include "simulation/SnapshotDelta.h"
#include "simulation/ElementClasses.h"
#include "simulation/ElementGraphics.h"
#include "simulation/ToolClasses.h"
#include "gui/game/DecorationTool.h"
#include "gui/interface/Engine.h"
#include "gui/dialogues/ErrorMessage.h"
#include <iostream>
#include <algorithm>
#include <optional>

HistoryEntry::~HistoryEntry()
{
	// * Needed because Snapshot and SnapshotDelta are incomplete types in GameModel.h,
	//   so the default dtor for ~HistoryEntry cannot be generated.
}

GameModel::GameModel():
	activeMenu(-1),
	currentBrush(0),
	currentUser(0, ""),
	toolStrength(1.0f),
	historyPosition(0),
	activeColourPreset(0),
	colourSelector(false),
	colour(255, 0, 0, 255),
	edgeMode(EDGE_VOID),
	ambientAirTemp(R_TEMP + 273.15f),
	decoSpace(DECOSPACE_SRGB)
{
	sim = new Simulation();
	sim->useLuaCallbacks = true;
	ren = new Renderer(sim);

	activeTools = &regularToolset[0];

	std::fill(decoToolset.begin(), decoToolset.end(), nullptr);
	std::fill(regularToolset.begin(), regularToolset.end(), nullptr);

	//Default render prefs
	ren->SetRenderMode({
		RENDER_FIRE,
		RENDER_EFFE,
		RENDER_BASC,
	});
	ren->SetDisplayMode({});
	ren->SetColourMode(0);

	//Load config into renderer
	auto &prefs = GlobalPrefs::Ref();
	ren->SetColourMode(prefs.Get("Renderer.ColourMode", 0U));

	auto displayModes = prefs.Get("Renderer.DisplayModes", std::vector<unsigned int>{});
	if (displayModes.size())
	{
		ren->SetDisplayMode(displayModes);
	}
	auto renderModes = prefs.Get("Renderer.RenderModes", std::vector<unsigned int>{});
	if (renderModes.size())
	{
		ren->SetRenderMode(renderModes);
	}

	ren->gravityFieldEnabled = prefs.Get("Renderer.GravityField", false);
	ren->decorations_enable = prefs.Get("Renderer.Decorations", true);

	//Load config into simulation
	edgeMode = prefs.Get("Simulation.EdgeMode", NUM_EDGEMODES, EDGE_VOID);
	sim->SetEdgeMode(edgeMode);
	ambientAirTemp = float(R_TEMP) + 273.15f;
	{
		auto temp = prefs.Get("Simulation.AmbientAirTemp", ambientAirTemp);
		if (MIN_TEMP <= temp && MAX_TEMP >= temp)
		{
			ambientAirTemp = temp;
		}
	}
	sim->air->ambientAirTemp = ambientAirTemp;
	decoSpace = prefs.Get("Simulation.DecoSpace", NUM_DECOSPACES, DECOSPACE_SRGB);
	sim->SetDecoSpace(decoSpace);
	int ngrav_enable = prefs.Get("Simulation.NewtonianGravity", NUM_GRAVMODES, GRAV_VERTICAL);
	if (ngrav_enable)
		sim->grav->start_grav_async();
	sim->aheat_enable = prefs.Get("Simulation.AmbientHeat", 0); // TODO: AmbientHeat enum
	sim->pretty_powder = prefs.Get("Simulation.PrettyPowder", 0); // TODO: PrettyPowder enum

	Favorite::Ref().LoadFavoritesFromPrefs();

	//Load last user
	if(Client::Ref().GetAuthUser().UserID)
	{
		currentUser = Client::Ref().GetAuthUser();
	}

	BuildMenus();

	perfectCircle = prefs.Get("PerfectCircleBrush", true);
	BuildBrushList();

	//Set default decoration colour
	unsigned char colourR = std::max(std::min(prefs.Get("Decoration.Red", 200), 255), 0);
	unsigned char colourG = std::max(std::min(prefs.Get("Decoration.Green", 100), 255), 0);
	unsigned char colourB = std::max(std::min(prefs.Get("Decoration.Blue", 50), 255), 0);
	unsigned char colourA = std::max(std::min(prefs.Get("Decoration.Alpha", 255), 255), 0);

	SetColourSelectorColour(ui::Colour(colourR, colourG, colourB, colourA));

	colourPresets.push_back(ui::Colour(255, 255, 255));
	colourPresets.push_back(ui::Colour(0, 255, 255));
	colourPresets.push_back(ui::Colour(255, 0, 255));
	colourPresets.push_back(ui::Colour(255, 255, 0));
	colourPresets.push_back(ui::Colour(255, 0, 0));
	colourPresets.push_back(ui::Colour(0, 255, 0));
	colourPresets.push_back(ui::Colour(0, 0, 255));
	colourPresets.push_back(ui::Colour(0, 0, 0));

	undoHistoryLimit = prefs.Get("Simulation.UndoHistoryLimit", 5U);
	// cap due to memory usage (this is about 3.4GB of RAM)
	if (undoHistoryLimit > 200)
		SetUndoHistoryLimit(200);

	mouseClickRequired = prefs.Get("MouseClickRequired", false);
	includePressure = prefs.Get("Simulation.IncludePressure", true);
	temperatureScale = prefs.Get("Renderer.TemperatureScale", 1); // TODO: TemperatureScale enum

	ClearSimulation();
}

GameModel::~GameModel()
{
	auto &prefs = GlobalPrefs::Ref();
	{
		//Save to config:
		Prefs::DeferWrite dw(prefs);
		prefs.Set("Renderer.ColourMode", ren->GetColourMode());
		prefs.Set("Renderer.DisplayModes", ren->GetDisplayMode());
		prefs.Set("Renderer.RenderModes", ren->GetRenderMode());
		prefs.Set("Renderer.GravityField", (bool)ren->gravityFieldEnabled);
		prefs.Set("Renderer.Decorations", (bool)ren->decorations_enable);
		prefs.Set("Renderer.DebugMode", ren->debugLines); //These two should always be equivalent, even though they are different things
		prefs.Set("Simulation.NewtonianGravity", sim->grav->IsEnabled());
		prefs.Set("Simulation.AmbientHeat", sim->aheat_enable);
		prefs.Set("Simulation.PrettyPowder", sim->pretty_powder);
		prefs.Set("Decoration.Red", (int)colour.Red);
		prefs.Set("Decoration.Green", (int)colour.Green);
		prefs.Set("Decoration.Blue", (int)colour.Blue);
		prefs.Set("Decoration.Alpha", (int)colour.Alpha);
	}

	for (size_t i = 0; i < menuList.size(); i++)
	{
		if (i == SC_FAVORITES)
			menuList[i]->ClearTools();
		delete menuList[i];
	}
	for (std::vector<Tool*>::iterator iter = extraElementTools.begin(), end = extraElementTools.end(); iter != end; ++iter)
	{
		delete *iter;
	}
	delete sim;
	delete ren;
	//if(activeTools)
	//	delete[] activeTools;
}

void GameModel::UpdateQuickOptions()
{
	for(std::vector<QuickOption*>::iterator iter = quickOptions.begin(), end = quickOptions.end(); iter != end; ++iter)
	{
		QuickOption * option = *iter;
		option->Update();
	}
}

void GameModel::BuildQuickOptionMenu(GameController * controller)
{
	for(std::vector<QuickOption*>::iterator iter = quickOptions.begin(), end = quickOptions.end(); iter != end; ++iter)
	{
		delete *iter;
	}
	quickOptions.clear();

	quickOptions.push_back(new SandEffectOption(this));
	quickOptions.push_back(new DrawGravOption(this));
	quickOptions.push_back(new DecorationsOption(this));
	quickOptions.push_back(new NGravityOption(this));
	quickOptions.push_back(new AHeatOption(this));
	quickOptions.push_back(new ConsoleShowOption(this, controller));

	notifyQuickOptionsChanged();
	UpdateQuickOptions();
}

void GameModel::BuildMenus()
{
	auto &sd = SimulationData::Ref();
	auto &elements = sd.elements;
	auto &builtinGol = SimulationData::builtinGol;
	int lastMenu = -1;
	if(activeMenu != -1)
		lastMenu = activeMenu;

	std::array<ByteString, NUM_TOOLINDICES> activeToolIdentifiers;
	if(regularToolset[0])
		activeToolIdentifiers[0] = regularToolset[0]->Identifier;
	if(regularToolset[1])
		activeToolIdentifiers[1] = regularToolset[1]->Identifier;
	if(regularToolset[2])
		activeToolIdentifiers[2] = regularToolset[2]->Identifier;
	if(regularToolset[3])
		activeToolIdentifiers[3] = regularToolset[3]->Identifier;

	//Empty current menus
	for (size_t i = 0; i < menuList.size(); i++)
	{
		if (i == SC_FAVORITES)
			menuList[i]->ClearTools();
		delete menuList[i];
	}
	menuList.clear();
	toolList.clear();

	for(std::vector<Tool*>::iterator iter = extraElementTools.begin(), end = extraElementTools.end(); iter != end; ++iter)
	{
		delete *iter;
	}
	extraElementTools.clear();
	elementTools.clear();

	//Create menus
	for (int i = 0; i < SC_TOTAL; i++)
	{
		menuList.push_back(new Menu(sd.msections[i].icon, sd.msections[i].name, sd.msections[i].doshow));
	}

	//Build menus from Simulation elements
	for(int i = 0; i < PT_NUM; i++)
	{
		if(elements[i].Enabled)
		{
			Tool * tempTool;
			if(i == PT_LIGH)
			{
				tempTool = new Element_LIGH_Tool(i, elements[i].Name, elements[i].Description, elements[i].Colour, elements[i].Identifier, elements[i].IconGenerator);
			}
			else if(i == PT_TESC)
			{
				tempTool = new Element_TESC_Tool(i, elements[i].Name, elements[i].Description, elements[i].Colour, elements[i].Identifier, elements[i].IconGenerator);
			}
			else if(i == PT_STKM || i == PT_FIGH || i == PT_STKM2)
			{
				tempTool = new PlopTool(i, elements[i].Name, elements[i].Description, elements[i].Colour, elements[i].Identifier, elements[i].IconGenerator);
			}
			else
			{
				tempTool = new ElementTool(i, elements[i].Name, elements[i].Description, elements[i].Colour, elements[i].Identifier, elements[i].IconGenerator);
			}

			if (elements[i].MenuSection >= 0 && elements[i].MenuSection < SC_TOTAL && elements[i].MenuVisible)
			{
				menuList[elements[i].MenuSection]->AddTool(tempTool);
			}
			else
			{
				extraElementTools.push_back(tempTool);
			}
			elementTools.push_back(tempTool);
		}
	}

	//Build menu for GOL types
	for(int i = 0; i < NGOL; i++)
	{
		Tool * tempTool = new ElementTool(PT_LIFE|PMAPID(i), builtinGol[i].name, builtinGol[i].description, builtinGol[i].colour, "DEFAULT_PT_LIFE_"+builtinGol[i].name.ToAscii());
		menuList[SC_LIFE]->AddTool(tempTool);
	}
	{
		auto &prefs = GlobalPrefs::Ref();
		auto customGOLTypes = prefs.Get("CustomGOL.Types", std::vector<ByteString>{});
		std::vector<ByteString> validatedCustomLifeTypes;
		std::vector<CustomGOLData> newCustomGol;
		bool removedAny = false;
		for (auto gol : customGOLTypes)
		{
			auto parts = gol.FromUtf8().PartitionBy(' ');
			if (parts.size() != 4)
			{
				removedAny = true;
				continue;
			}
			CustomGOLData gd;
			gd.nameString = parts[0];
			gd.ruleString = parts[1];
			auto &colour1String = parts[2];
			auto &colour2String = parts[3];
			if (!ValidateGOLName(gd.nameString))
			{
				removedAny = true;
				continue;
			}
			gd.rule = ParseGOLString(gd.ruleString);
			if (gd.rule == -1)
			{
				removedAny = true;
				continue;
			}
			try
			{
				gd.colour1 = colour1String.ToNumber<int>();
				gd.colour2 = colour2String.ToNumber<int>();
			}
			catch (std::exception &)
			{
				removedAny = true;
				continue;
			}
			newCustomGol.push_back(gd);
			validatedCustomLifeTypes.push_back(gol);
		}
		if (removedAny)
		{
			// All custom rules that fail validation will be removed
			prefs.Set("CustomGOL.Types", validatedCustomLifeTypes);
		}
		for (auto &gd : newCustomGol)
		{
			Tool * tempTool = new ElementTool(PT_LIFE|PMAPID(gd.rule), gd.nameString, "Custom GOL type: " + gd.ruleString, RGB<uint8_t>::Unpack(gd.colour1), "DEFAULT_PT_LIFECUST_"+gd.nameString.ToAscii(), NULL);
			menuList[SC_LIFE]->AddTool(tempTool);
		}
		sd.SetCustomGOL(newCustomGol);
	}

	//Build other menus from wall data
	for(int i = 0; i < UI_WALLCOUNT; i++)
	{
		Tool * tempTool = new WallTool(i, sd.wtypes[i].descs, sd.wtypes[i].colour, sd.wtypes[i].identifier, sd.wtypes[i].textureGen);
		menuList[SC_WALL]->AddTool(tempTool);
	}

	//Build menu for tools
	for (size_t i = 0; i < sd.tools.size(); i++)
	{
		Tool *tempTool = new Tool(
			i,
			sd.tools[i].Name,
			sd.tools[i].Description,
			sd.tools[i].Colour,
			sd.tools[i].Identifier
		);
		menuList[SC_TOOL]->AddTool(tempTool);
	}
	//Add special sign and prop tools
	menuList[SC_TOOL]->AddTool(new WindTool());
	menuList[SC_TOOL]->AddTool(new PropertyTool(*this));
	menuList[SC_TOOL]->AddTool(new SignTool(*this));
	menuList[SC_TOOL]->AddTool(new SampleTool(*this));
	menuList[SC_LIFE]->AddTool(new GOLTool(*this));

	//Add decoration tools to menu
	menuList[SC_DECO]->AddTool(new DecorationTool(*ren, DECO_ADD, "ADD", "Colour blending: Add.", 0x000000_rgb, "DEFAULT_DECOR_ADD"));
	menuList[SC_DECO]->AddTool(new DecorationTool(*ren, DECO_SUBTRACT, "SUB", "Colour blending: Subtract.", 0x000000_rgb, "DEFAULT_DECOR_SUB"));
	menuList[SC_DECO]->AddTool(new DecorationTool(*ren, DECO_MULTIPLY, "MUL", "Colour blending: Multiply.", 0x000000_rgb, "DEFAULT_DECOR_MUL"));
	menuList[SC_DECO]->AddTool(new DecorationTool(*ren, DECO_DIVIDE, "DIV", "Colour blending: Divide." , 0x000000_rgb, "DEFAULT_DECOR_DIV"));
	menuList[SC_DECO]->AddTool(new DecorationTool(*ren, DECO_SMUDGE, "SMDG", "Smudge tool, blends surrounding deco together.", 0x000000_rgb, "DEFAULT_DECOR_SMDG"));
	menuList[SC_DECO]->AddTool(new DecorationTool(*ren, DECO_CLEAR, "CLR", "Erase any set decoration.", 0x000000_rgb, "DEFAULT_DECOR_CLR"));
	menuList[SC_DECO]->AddTool(new DecorationTool(*ren, DECO_DRAW, "SET", "Draw decoration (No blending).", 0x000000_rgb, "DEFAULT_DECOR_SET"));
	SetColourSelectorColour(colour); // update tool colors
	decoToolset[0] = GetToolFromIdentifier("DEFAULT_DECOR_SET");
	decoToolset[1] = GetToolFromIdentifier("DEFAULT_DECOR_CLR");
	decoToolset[2] = GetToolFromIdentifier("DEFAULT_UI_SAMPLE");
	decoToolset[3] = GetToolFromIdentifier("DEFAULT_PT_NONE");

	regularToolset[0] = GetToolFromIdentifier(activeToolIdentifiers[0]);
	regularToolset[1] = GetToolFromIdentifier(activeToolIdentifiers[1]);
	regularToolset[2] = GetToolFromIdentifier(activeToolIdentifiers[2]);
	regularToolset[3] = GetToolFromIdentifier(activeToolIdentifiers[3]);

	//Set default tools
	if (!regularToolset[0])
		regularToolset[0] = GetToolFromIdentifier("DEFAULT_PT_DUST");
	if (!regularToolset[1])
		regularToolset[1] = GetToolFromIdentifier("DEFAULT_PT_NONE");
	if (!regularToolset[2])
		regularToolset[2] = GetToolFromIdentifier("DEFAULT_UI_SAMPLE");
	if (!regularToolset[3])
		regularToolset[3] = GetToolFromIdentifier("DEFAULT_PT_NONE");

	lastTool = activeTools[0];

	//Set default menu
	activeMenu = SC_POWDERS;

	if(lastMenu != -1)
		activeMenu = lastMenu;

	if(activeMenu != -1)
		toolList = menuList[activeMenu]->GetToolList();
	else
		toolList = std::vector<Tool*>();

	notifyMenuListChanged();
	notifyToolListChanged();
	notifyActiveToolsChanged();
	notifyLastToolChanged();

	//Build menu for favorites
	BuildFavoritesMenu();
}

void GameModel::BuildFavoritesMenu()
{
	menuList[SC_FAVORITES]->ClearTools();

	std::vector<ByteString> favList = Favorite::Ref().GetFavoritesList();
	for (size_t i = 0; i < favList.size(); i++)
	{
		Tool *tool = GetToolFromIdentifier(favList[i]);
		if (tool)
			menuList[SC_FAVORITES]->AddTool(tool);
	}

	if (activeMenu == SC_FAVORITES)
		toolList = menuList[SC_FAVORITES]->GetToolList();

	notifyMenuListChanged();
	notifyToolListChanged();
	notifyActiveToolsChanged();
	notifyLastToolChanged();
}

void GameModel::BuildBrushList()
{
	ui::Point radius{ 4, 4 };
	if (brushList.size())
		radius = brushList[currentBrush]->GetRadius();
	brushList.clear();

	brushList.push_back(std::make_unique<EllipseBrush>(perfectCircle));
	brushList.push_back(std::make_unique<RectangleBrush>());
	brushList.push_back(std::make_unique<TriangleBrush>());

	//Load more from brushes folder
	for (ByteString brushFile : Platform::DirectorySearch(BRUSH_DIR, "", { ".ptb" }))
	{
		std::vector<char> brushData;
		if (!Platform::ReadFile(brushData, ByteString::Build(BRUSH_DIR, PATH_SEP_CHAR, brushFile)))
		{
			std::cout << "Brushes: Skipping " << brushFile << ". Could not open" << std::endl;
			continue;
		}
		auto dimension = size_t(std::sqrt(brushData.size()));
		if (dimension * dimension != brushData.size())
		{
			std::cout << "Brushes: Skipping " << brushFile << ". Invalid bitmap size" << std::endl;
			continue;
		}
		brushList.push_back(std::make_unique<BitmapBrush>(ui::Point(dimension, dimension), reinterpret_cast<unsigned char const *>(brushData.data())));
	}

	brushList[currentBrush]->SetRadius(radius);
	notifyBrushChanged();
}

Tool *GameModel::GetToolFromIdentifier(ByteString const &identifier)
{
	for (auto *menu : menuList)
	{
		for (auto *tool : menu->GetToolList())
		{
			if (identifier == tool->Identifier)
			{
				return tool;
			}
		}
	}
	for (auto *extra : extraElementTools)
	{
		if (identifier == extra->Identifier)
		{
			return extra;
		}
	}
	return nullptr;
}

void GameModel::SetEdgeMode(int edgeMode)
{
	this->edgeMode = edgeMode;
	sim->SetEdgeMode(edgeMode);
}

int GameModel::GetEdgeMode()
{
	return this->edgeMode;
}

void GameModel::SetTemperatureScale(int temperatureScale)
{
	this->temperatureScale = temperatureScale;
}

void GameModel::SetAmbientAirTemperature(float ambientAirTemp)
{
	this->ambientAirTemp = ambientAirTemp;
	sim->air->ambientAirTemp = ambientAirTemp;
}

float GameModel::GetAmbientAirTemperature()
{
	return this->ambientAirTemp;
}

void GameModel::SetDecoSpace(int decoSpace)
{
	sim->SetDecoSpace(decoSpace);
	this->decoSpace = sim->deco_space;
}

int GameModel::GetDecoSpace()
{
	return this->decoSpace;
}

// * SnapshotDelta d is the difference between the two Snapshots A and B (i.e. d = B - A)
//   if auto d = SnapshotDelta::FromSnapshots(A, B). In this case, a Snapshot that is
//   identical to B can be constructed from d and A via d.Forward(A) (i.e. B = A + d)
//   and a Snapshot that is identical to A can be constructed from d and B via
//   d.Restore(B) (i.e. A = B - d). SnapshotDeltas often consume less memory than Snapshots,
//   although pathological cases of pairs of Snapshots exist, the SnapshotDelta constructed
//   from which actually consumes more than the two snapshots combined.
// * GameModel::history is an N-item deque of HistoryEntry structs, each of which owns either
//   a SnapshotDelta, except for history[N-1], which always owns a Snapshot. A logical Snapshot
//   accompanies each item in GameModel::history. This logical Snapshot may or may not be
//   materialised (present in memory). If an item owns an actual Snapshot, the aforementioned
//   logical Snapshot is this materialised Snapshot. If, however, an item owns a SnapshotDelta d,
//   the accompanying logical Snapshot A is the Snapshot obtained via A = d.Restore(B), where B
//   is the logical Snapshot accompanying the next (at an index that is one higher than the
//   index of this item) item in history. Slightly more visually:
//
//      i   |    history[i]   |  the logical Snapshot   | relationships |
//          |                 | accompanying history[i] |               |
//   -------|-----------------|-------------------------|---------------|
//          |                 |                         |               |
//    N - 1 |   Snapshot A    |       Snapshot A        |            A  |
//          |                 |                         |           /   |
//    N - 2 | SnapshotDelta b |       Snapshot B        |  B+b=A   b-B  |
//          |                 |                         |           /   |
//    N - 3 | SnapshotDelta c |       Snapshot C        |  C+c=B   c-C  |
//          |                 |                         |           /   |
//    N - 4 | SnapshotDelta d |       Snapshot D        |  D+d=C   d-D  |
//          |                 |                         |           /   |
//     ...  |      ...        |          ...            |   ...    ...  |
//
// * GameModel::historyPosition is an integer in the closed range 0 to N, which is decremented
//   by GameModel::HistoryRestore and incremented by GameModel::HistoryForward, by 1 at a time.
//   GameModel::historyCurrent "follows" historyPosition such that it always holds a Snapshot
//   that is identical to the logical Snapshot of history[historyPosition], except when
//   historyPosition = N, in which case it's empty. This following behaviour is achieved either
//   by "stepping" historyCurrent by Forwarding and Restoring it via the SnapshotDelta in
//   history[historyPosition], cloning the Snapshot in history[historyPosition] into it if
//   historyPosition = N-1, or clearing if it historyPosition = N.
// * GameModel::historyCurrent is lost when a new Snapshot item is pushed into GameModel::history.
//   This item appears wherever historyPosition currently points, and every other item above it
//   is deleted. If historyPosition is below N, this gets rid of the Snapshot in history[N-1].
//   Thus, N is set to historyPosition, after which the new Snapshot is pushed and historyPosition
//   is incremented to the new N.
// * Pushing a new Snapshot into the history is a bit involved:
//   * If there are no history entries yet, the new Snapshot is simply placed into GameModel::history.
//     From now on, we consider cases in which GameModel::history is originally not empty.
//
//     === after pushing Snapshot A' into the history
//  
//        i   |    history[i]   |  the logical Snapshot   | relationships |
//            |                 | accompanying history[i] |               |
//     -------|-----------------|-------------------------|---------------|
//            |                 |                         |               |
//        0   |   Snapshot A    |       Snapshot A        |            A  |
//
//   * If there were discarded history entries (i.e. the user decided to continue from some state
//     which they arrived to via at least one Ctrl+Z), history[N-2] is a SnapshotDelta that when
//     Forwarded with the logical Snapshot of history[N-2] yields the logical Snapshot of history[N-1]
//     from before the new item was pushed. This is not what we want, so we replace it with a
//     SnapshotDelta that is the difference between the logical Snapshot of history[N-2] and the
//     Snapshot freshly placed in history[N-1].
//
//     === after pushing Snapshot A' into the history
//  
//        i   |    history[i]   |  the logical Snapshot   | relationships |
//            |                 | accompanying history[i] |               |
//     -------|-----------------|-------------------------|---------------|
//            |                 |                         |               |
//      N - 1 |   Snapshot A'   |       Snapshot A'       |            A' | b needs to be replaced with b',
//            |                 |                         |           /   | B+b'=A'; otherwise we'd run
//      N - 2 | SnapshotDelta b |       Snapshot B        |  B+b=A   b-B  | into problems when trying to
//            |                 |                         |           /   | reconstruct B from A' and b
//      N - 3 | SnapshotDelta c |       Snapshot C        |  C+c=B   c-C  | in HistoryRestore.
//            |                 |                         |           /   |
//      N - 4 | SnapshotDelta d |       Snapshot D        |  D+d=C   d-D  |
//            |                 |                         |           /   |
//       ...  |      ...        |          ...            |   ...    ...  |
//  
//     === after replacing b with b'
//  
//        i   |    history[i]   |  the logical Snapshot   | relationships |
//            |                 | accompanying history[i] |               |
//     -------|-----------------|-------------------------|---------------|
//            |                 |                         |               |
//      N - 1 |   Snapshot A'   |       Snapshot A'       |            A' |
//            |                 |                         |           /   |
//      N - 2 | SnapshotDelta b'|       Snapshot B        | B+b'=A' b'-B  |
//            |                 |                         |           /   |
//      N - 3 | SnapshotDelta c |       Snapshot C        |  C+c=B   c-C  |
//            |                 |                         |           /   |
//      N - 4 | SnapshotDelta d |       Snapshot D        |  D+d=C   d-D  |
//            |                 |                         |           /   |
//       ...  |      ...        |          ...            |   ...    ...  |
//  
//   * If there weren't any discarded history entries, history[N-2] is now also a Snapshot. Since
//     the freshly pushed Snapshot in history[N-1] should be the only Snapshot in history, this is
//     replaced with the SnapshotDelta that is the difference between history[N-2] and the Snapshot
//     freshly placed in history[N-1].
//
//     === after pushing Snapshot A' into the history
//
//        i   |    history[i]   |  the logical Snapshot   | relationships |
//            |                 | accompanying history[i] |               |
//     -------|-----------------|-------------------------|---------------|
//            |                 |                         |               |
//      N - 1 |   Snapshot A'   |       Snapshot A'       |            A' | A needs to be converted to a,
//            |                 |                         |               | otherwise Snapshots would litter
//      N - 1 |   Snapshot A    |       Snapshot A        |            A  | GameModel::history, which we
//            |                 |                         |           /   | want to avoid because they
//      N - 2 | SnapshotDelta b |       Snapshot B        |  B+b=A   b-B  | waste a ton of memory
//            |                 |                         |           /   |
//      N - 3 | SnapshotDelta c |       Snapshot C        |  C+c=B   c-C  |
//            |                 |                         |           /   |
//      N - 4 | SnapshotDelta d |       Snapshot D        |  D+d=C   d-D  |
//            |                 |                         |           /   |
//       ...  |      ...        |          ...            |   ...    ...  |
//
//     === after replacing A with a
//
//        i   |    history[i]   |  the logical Snapshot   | relationships |
//            |                 | accompanying history[i] |               |
//     -------|-----------------|-------------------------|---------------|
//            |                 |                         |               |
//      N - 1 |   Snapshot A'   |       Snapshot A'       |            A' |
//            |                 |                         |           /   |
//      N - 1 | SnapshotDelta a |       Snapshot A        |  A+a=A'  a-A  |
//            |                 |                         |           /   |
//      N - 2 | SnapshotDelta b |       Snapshot B        |  B+b=A   b-B  |
//            |                 |                         |           /   |
//      N - 3 | SnapshotDelta c |       Snapshot C        |  C+c=B   c-C  |
//            |                 |                         |           /   |
//      N - 4 | SnapshotDelta d |       Snapshot D        |  D+d=C   d-D  |
//            |                 |                         |           /   |
//       ...  |      ...        |          ...            |   ...    ...  |
//
//   * After all this, the front of the deque is truncated such that there are on more than
//     undoHistoryLimit entries left.

const Snapshot *GameModel::HistoryCurrent() const
{
	return historyCurrent.get();
}

bool GameModel::HistoryCanRestore() const
{
	return historyPosition > 0U;
}

void GameModel::HistoryRestore()
{
	if (!HistoryCanRestore())
	{
		return;
	}
	historyPosition -= 1U;
	if (history[historyPosition].snap)
	{
		historyCurrent = std::make_unique<Snapshot>(*history[historyPosition].snap);
	}
	else
	{
		historyCurrent = history[historyPosition].delta->Restore(*historyCurrent);
	}
}

bool GameModel::HistoryCanForward() const
{
	return historyPosition < history.size();
}

void GameModel::HistoryForward()
{
	if (!HistoryCanForward())
	{
		return;
	}
	historyPosition += 1U;
	if (historyPosition == history.size())
	{
		historyCurrent = nullptr;
	}
	else if (history[historyPosition].snap)
	{
		historyCurrent = std::make_unique<Snapshot>(*history[historyPosition].snap);
	}
	else
	{
		historyCurrent = history[historyPosition - 1U].delta->Forward(*historyCurrent);
	}
}

void GameModel::HistoryPush(std::unique_ptr<Snapshot> last)
{
	Snapshot *rebaseOnto = nullptr;
	if (historyPosition)
	{
		rebaseOnto = history.back().snap.get();
		if (historyPosition < history.size())
		{
			historyCurrent = history[historyPosition - 1U].delta->Restore(*historyCurrent);
			rebaseOnto = historyCurrent.get();
		}
	}
	while (historyPosition < history.size())
	{
		history.pop_back();
	}
	if (rebaseOnto)
	{
		auto &prev = history.back();
		prev.delta = SnapshotDelta::FromSnapshots(*rebaseOnto, *last);
		prev.snap.reset();
	}
	history.emplace_back();
	history.back().snap = std::move(last);
	historyPosition += 1U;
	historyCurrent.reset();
	while (undoHistoryLimit < history.size())
	{
		history.pop_front();
		historyPosition -= 1U;
	}
}

unsigned int GameModel::GetUndoHistoryLimit()
{
	return undoHistoryLimit;
}

void GameModel::SetUndoHistoryLimit(unsigned int undoHistoryLimit_)
{
	undoHistoryLimit = undoHistoryLimit_;
	GlobalPrefs::Ref().Set("Simulation.UndoHistoryLimit", undoHistoryLimit);
}

void GameModel::SetVote(int direction)
{
	queuedVote = direction;
}

void GameModel::Tick()
{
	if (execVoteRequest && execVoteRequest->CheckDone())
	{
		try
		{
			execVoteRequest->Finish();
			currentSave->vote = execVoteRequest->Direction();
			notifySaveChanged();
		}
		catch (const http::RequestError &ex)
		{
			new ErrorMessage("Error while voting", ByteString(ex.what()).FromUtf8());
		}
		execVoteRequest.reset();
	}
	if (!execVoteRequest && queuedVote)
	{
		if (currentSave)
		{
			execVoteRequest = std::make_unique<http::ExecVoteRequest>(currentSave->GetID(), *queuedVote);
			execVoteRequest->Start();
		}
		queuedVote.reset();
	}
}

Brush &GameModel::GetBrush()
{
	return *brushList[currentBrush];
}

Brush *GameModel::GetBrushByID(int i)
{
	if (i >= 0 && i < (int)brushList.size())
		return brushList[i].get();
	else
		return nullptr;
}

int GameModel::GetBrushID()
{
	return currentBrush;
}

void GameModel::SetBrushID(int i)
{
	auto prevRadius = brushList[currentBrush]->GetRadius();
	currentBrush = i%brushList.size();
	brushList[currentBrush]->SetRadius(prevRadius);
	notifyBrushChanged();
}

void GameModel::AddObserver(GameView * observer){
	observers.push_back(observer);

	observer->NotifySimulationChanged(this);
	observer->NotifyRendererChanged(this);
	observer->NotifyPausedChanged(this);
	observer->NotifySaveChanged(this);
	observer->NotifyBrushChanged(this);
	observer->NotifyMenuListChanged(this);
	observer->NotifyToolListChanged(this);
	observer->NotifyUserChanged(this);
	observer->NotifyZoomChanged(this);
	observer->NotifyColourSelectorVisibilityChanged(this);
	observer->NotifyColourSelectorColourChanged(this);
	observer->NotifyColourPresetsChanged(this);
	observer->NotifyColourActivePresetChanged(this);
	observer->NotifyQuickOptionsChanged(this);
	observer->NotifyLastToolChanged(this);
	UpdateQuickOptions();
}

void GameModel::SetToolStrength(float value)
{
	toolStrength = value;
}

float GameModel::GetToolStrength()
{
	return toolStrength;
}

void GameModel::SetActiveMenu(int menuID)
{
	activeMenu = menuID;
	toolList = menuList[menuID]->GetToolList();
	notifyToolListChanged();

	if(menuID == SC_DECO)
	{
		if(activeTools != &decoToolset[0])
		{
			activeTools = &decoToolset[0];
			notifyActiveToolsChanged();
		}
	}
	else
	{
		if(activeTools != &regularToolset[0])
		{
			activeTools = &regularToolset[0];
			notifyActiveToolsChanged();
		}
	}
}

std::vector<Tool*> GameModel::GetUnlistedTools()
{
	return extraElementTools;
}

std::vector<Tool*> GameModel::GetToolList()
{
	return toolList;
}

int GameModel::GetActiveMenu()
{
	return activeMenu;
}

//Get an element tool from an element ID
Tool * GameModel::GetElementTool(int elementID)
{
	for(std::vector<Tool*>::iterator iter = elementTools.begin(), end = elementTools.end(); iter != end; ++iter)
	{
		if((*iter)->ToolID == elementID)
			return *iter;
	}
	return NULL;
}

Tool * GameModel::GetActiveTool(int selection)
{
	return activeTools[selection];
}

void GameModel::SetActiveTool(int selection, Tool * tool)
{
	activeTools[selection] = tool;
	notifyActiveToolsChanged();
}

std::vector<QuickOption*> GameModel::GetQuickOptions()
{
	return quickOptions;
}

std::vector<Menu*> GameModel::GetMenuList()
{
	return menuList;
}

SaveInfo *GameModel::GetSave() // non-owning
{
	return currentSave.get();
}

std::unique_ptr<SaveInfo> GameModel::TakeSave()
{
	// we don't notify listeners because we'll get a new save soon anyway
	return std::move(currentSave);
}

void GameModel::SaveToSimParameters(const GameSave &saveData)
{
	SetPaused(saveData.paused | GetPaused());
	sim->gravityMode = saveData.gravityMode;
	sim->customGravityX = saveData.customGravityX;
	sim->customGravityY = saveData.customGravityY;
	sim->air->airMode = saveData.airMode;
	sim->air->ambientAirTemp = saveData.ambientAirTemp;
	sim->edgeMode = saveData.edgeMode;
	sim->legacy_enable = saveData.legacyEnable;
	sim->water_equal_test = saveData.waterEEnabled;
	sim->aheat_enable = saveData.aheatEnable;
	if (saveData.gravityEnable && !sim->grav->IsEnabled())
	{
		sim->grav->start_grav_async();
	}
	else if (!saveData.gravityEnable && sim->grav->IsEnabled())
	{
		sim->grav->stop_grav_async();
	}
	sim->frameCount = saveData.frameCount;
	if (saveData.hasRngState)
	{
		sim->rng.state(saveData.rngState);
	}
	else
	{
		sim->rng = RNG();
	}
	sim->ensureDeterminism = saveData.ensureDeterminism;
}

void GameModel::SetSave(std::unique_ptr<SaveInfo> newSave, bool invertIncludePressure)
{
	currentSave = std::move(newSave);
	currentFile.reset();

	if (currentSave && currentSave->GetGameSave())
	{
		auto *saveData = currentSave->GetGameSave();
		SaveToSimParameters(*saveData);
		sim->clear_sim();
		ren->ClearAccumulation();
		sim->Load(saveData, !invertIncludePressure, { 0, 0 });
		// This save was created before logging existed
		// Add in the correct info
		if (saveData->authors.size() == 0)
		{
			auto gameSave = currentSave->TakeGameSave();
			gameSave->authors["type"] = "save";
			gameSave->authors["id"] = currentSave->id;
			gameSave->authors["username"] = currentSave->userName;
			gameSave->authors["title"] = currentSave->name.ToUtf8();
			gameSave->authors["description"] = currentSave->Description.ToUtf8();
			gameSave->authors["published"] = (int)currentSave->Published;
			gameSave->authors["date"] = (Json::Value::UInt64)currentSave->updatedDate;
			currentSave->SetGameSave(std::move(gameSave));
		}
		// This save was probably just created, and we didn't know the ID when creating it
		// Update with the proper ID
		else if (saveData->authors.get("id", -1) == 0 || saveData->authors.get("id", -1) == -1)
		{
			auto gameSave = currentSave->TakeGameSave();
			gameSave->authors["id"] = currentSave->id;
			currentSave->SetGameSave(std::move(gameSave));
		}
		Client::Ref().OverwriteAuthorInfo(saveData->authors);
	}
	notifySaveChanged();
	UpdateQuickOptions();
}

const SaveFile *GameModel::GetSaveFile() const
{
	return currentFile.get();
}

std::unique_ptr<SaveFile> GameModel::TakeSaveFile()
{
	// we don't notify listeners because we'll get a new save soon anyway
	return std::move(currentFile);
}

void GameModel::SetSaveFile(std::unique_ptr<SaveFile> newSave, bool invertIncludePressure)
{
	currentFile = std::move(newSave);
	currentSave.reset();

	if (currentFile && currentFile->GetGameSave())
	{
		auto *saveData = currentFile->GetGameSave();
		SaveToSimParameters(*saveData);
		sim->clear_sim();
		ren->ClearAccumulation();
		sim->Load(saveData, !invertIncludePressure, { 0, 0 });
		Client::Ref().OverwriteAuthorInfo(saveData->authors);
	}

	notifySaveChanged();
	UpdateQuickOptions();
}

Simulation * GameModel::GetSimulation()
{
	return sim;
}

Renderer * GameModel::GetRenderer()
{
	return ren;
}

User GameModel::GetUser()
{
	return currentUser;
}

Tool * GameModel::GetLastTool()
{
	return lastTool;
}

void GameModel::SetLastTool(Tool * newTool)
{
	if(lastTool != newTool)
	{
		lastTool = newTool;
		notifyLastToolChanged();
	}
}

void GameModel::SetZoomEnabled(bool enabled)
{
	ren->zoomEnabled = enabled;
	notifyZoomChanged();
}

bool GameModel::GetZoomEnabled()
{
	return ren->zoomEnabled;
}

void GameModel::SetZoomPosition(ui::Point position)
{
	ren->zoomScopePosition = position;
	notifyZoomChanged();
}

ui::Point GameModel::GetZoomPosition()
{
	return ren->zoomScopePosition;
}

bool GameModel::MouseInZoom(ui::Point position)
{
	if (!GetZoomEnabled())
		return false;

	int zoomFactor = GetZoomFactor();
	ui::Point zoomWindowPosition = GetZoomWindowPosition();
	ui::Point zoomWindowSize = ui::Point(GetZoomSize()*zoomFactor, GetZoomSize()*zoomFactor);

	if (position.X >= zoomWindowPosition.X && position.Y >= zoomWindowPosition.Y && position.X < zoomWindowPosition.X+zoomWindowSize.X && position.Y < zoomWindowPosition.Y+zoomWindowSize.Y)
		return true;
	return false;
}

ui::Point GameModel::AdjustZoomCoords(ui::Point position)
{
	if (!GetZoomEnabled())
		return position;

	int zoomFactor = GetZoomFactor();
	ui::Point zoomWindowPosition = GetZoomWindowPosition();
	ui::Point zoomWindowSize = ui::Point(GetZoomSize()*zoomFactor, GetZoomSize()*zoomFactor);

	if (position.X >= zoomWindowPosition.X && position.Y >= zoomWindowPosition.Y && position.X < zoomWindowPosition.X+zoomWindowSize.X && position.Y < zoomWindowPosition.Y+zoomWindowSize.Y)
		return ((position-zoomWindowPosition)/GetZoomFactor())+GetZoomPosition();
	return position;
}

void GameModel::SetZoomWindowPosition(ui::Point position)
{
	ren->zoomWindowPosition = position;
	notifyZoomChanged();
}

ui::Point GameModel::GetZoomWindowPosition()
{
	return ren->zoomWindowPosition;
}

void GameModel::SetZoomSize(int size)
{
	ren->zoomScopeSize = size;
	notifyZoomChanged();
}

int GameModel::GetZoomSize()
{
	return ren->zoomScopeSize;
}

void GameModel::SetZoomFactor(int factor)
{
	ren->ZFACTOR = factor;
	notifyZoomChanged();
}

int GameModel::GetZoomFactor()
{
	return ren->ZFACTOR;
}

void GameModel::SetActiveColourPreset(size_t preset)
{
	if (activeColourPreset-1 != preset)
		activeColourPreset = preset+1;
	else
	{
		activeTools[0] = GetToolFromIdentifier("DEFAULT_DECOR_SET");
		notifyActiveToolsChanged();
	}
	notifyColourActivePresetChanged();
}

size_t GameModel::GetActiveColourPreset()
{
	return activeColourPreset-1;
}

void GameModel::SetPresetColour(ui::Colour colour)
{
	if (activeColourPreset > 0 && activeColourPreset <= colourPresets.size())
	{
		colourPresets[activeColourPreset-1] = colour;
		notifyColourPresetsChanged();
	}
}

std::vector<ui::Colour> GameModel::GetColourPresets()
{
	return colourPresets;
}

void GameModel::SetColourSelectorVisibility(bool visibility)
{
	if(colourSelector != visibility)
	{
		colourSelector = visibility;
		notifyColourSelectorVisibilityChanged();
	}
}

bool GameModel::GetColourSelectorVisibility()
{
	return colourSelector;
}

void GameModel::SetColourSelectorColour(ui::Colour colour_)
{
	colour = colour_;

	std::vector<Tool*> tools = GetMenuList()[SC_DECO]->GetToolList();
	for (auto tool : tools)
		static_cast<DecorationTool *>(tool)->Colour = colour;

	notifyColourSelectorColourChanged();
}

ui::Colour GameModel::GetColourSelectorColour()
{
	return colour;
}

void GameModel::SetUser(User user)
{
	currentUser = user;
	//Client::Ref().SetAuthUser(user);
	notifyUserChanged();
}

void GameModel::SetPaused(bool pauseState)
{
	if (!pauseState && sim->debug_nextToUpdate > 0)
	{
		String logmessage = String::Build("Updated particles from #", sim->debug_nextToUpdate, " to end due to unpause");
		UpdateUpTo(NPART);
		Log(logmessage, false);
	}

	sim->sys_pause = pauseState?1:0;
	notifyPausedChanged();
}

bool GameModel::GetPaused()
{
	return sim->sys_pause?true:false;
}

void GameModel::SetDecoration(bool decorationState)
{
	if (ren->decorations_enable != (decorationState?1:0))
	{
		ren->decorations_enable = decorationState?1:0;
		notifyDecorationChanged();
		UpdateQuickOptions();
		if (decorationState)
			SetInfoTip("Decorations Layer: On");
		else
			SetInfoTip("Decorations Layer: Off");
	}
}

bool GameModel::GetDecoration()
{
	return ren->decorations_enable?true:false;
}

void GameModel::SetAHeatEnable(bool aHeat)
{
	sim->aheat_enable = aHeat;
	UpdateQuickOptions();
	if (aHeat)
		SetInfoTip("Ambient Heat: On");
	else
		SetInfoTip("Ambient Heat: Off");
}

bool GameModel::GetAHeatEnable()
{
	return sim->aheat_enable;
}

void GameModel::ResetAHeat()
{
	sim->air->ClearAirH();
}

void GameModel::SetNewtonianGravity(bool newtonainGravity)
{
    if (newtonainGravity)
    {
        sim->grav->start_grav_async();
        SetInfoTip("Newtonian Gravity: On");
    }
    else
    {
        sim->grav->stop_grav_async();
        SetInfoTip("Newtonian Gravity: Off");
    }
    UpdateQuickOptions();
}

bool GameModel::GetNewtonianGrvity()
{
    return sim->grav->IsEnabled();
}

void GameModel::ShowGravityGrid(bool showGrid)
{
	ren->gravityFieldEnabled = showGrid;
	if (showGrid)
		SetInfoTip("Gravity Grid: On");
	else
		SetInfoTip("Gravity Grid: Off");
}

bool GameModel::GetGravityGrid()
{
	return ren->gravityFieldEnabled;
}

void GameModel::FrameStep(int frames)
{
	sim->framerender += frames;
}

void GameModel::ClearSimulation()
{
	//Load defaults
	sim->gravityMode = GRAV_VERTICAL;
	sim->customGravityX = 0.0f;
	sim->customGravityY = 0.0f;
	sim->air->airMode = AIR_ON;
	sim->legacy_enable = false;
	sim->water_equal_test = false;
	sim->SetEdgeMode(edgeMode);
	sim->air->ambientAirTemp = ambientAirTemp;

	sim->clear_sim();
	ren->ClearAccumulation();
	Client::Ref().ClearAuthorInfo();

	notifySaveChanged();
	UpdateQuickOptions();
}

void GameModel::SetPlaceSave(std::unique_ptr<GameSave> save)
{
	transformedPlaceSave.reset();
	placeSave = std::move(save);
	notifyPlaceSaveChanged();
	if (placeSave && placeSave->missingElements)
	{
		Log("Paste content has missing custom elements", false);
	}
}

void GameModel::TransformPlaceSave(Mat2<int> transform, Vec2<int> nudge)
{
	if (placeSave)
	{
		transformedPlaceSave = std::make_unique<GameSave>(*placeSave);
		transformedPlaceSave->Transform(transform, nudge);
	}
	notifyTransformedPlaceSaveChanged();
}

void GameModel::SetClipboard(std::unique_ptr<GameSave> save)
{
	Clipboard::SetClipboardData(std::move(save));
}

const GameSave *GameModel::GetClipboard() const
{
	return Clipboard::GetClipboardData();
}

const GameSave *GameModel::GetTransformedPlaceSave() const
{
	return transformedPlaceSave.get();
}

void GameModel::Log(String message, bool printToFile)
{
	consoleLog.push_front(message);
	if(consoleLog.size()>100)
		consoleLog.pop_back();
	notifyLogChanged(message);
	if (printToFile)
		std::cout << message.ToUtf8() << std::endl;
}

std::deque<String> GameModel::GetLog()
{
	return consoleLog;
}

std::vector<Notification*> GameModel::GetNotifications()
{
	return notifications;
}

void GameModel::AddNotification(Notification * notification)
{
	notifications.push_back(notification);
	notifyNotificationsChanged();
}

void GameModel::RemoveNotification(Notification * notification)
{
	for(std::vector<Notification*>::iterator iter = notifications.begin(); iter != notifications.end(); ++iter)
	{
		if(*iter == notification)
		{
			delete *iter;
			notifications.erase(iter);
			break;
		}
	}
	notifyNotificationsChanged();
}

void GameModel::SetToolTip(String text)
{
	toolTip = text;
	notifyToolTipChanged();
}

void GameModel::SetInfoTip(String text)
{
	infoTip = text;
	notifyInfoTipChanged();
}

String GameModel::GetToolTip()
{
	return toolTip;
}

String GameModel::GetInfoTip()
{
	return infoTip;
}

void GameModel::notifyNotificationsChanged()
{
	for (std::vector<GameView*>::iterator iter = observers.begin(); iter != observers.end(); ++iter)
	{
		(*iter)->NotifyNotificationsChanged(this);
	}
}

void GameModel::notifyColourPresetsChanged()
{
	for (std::vector<GameView*>::iterator iter = observers.begin(); iter != observers.end(); ++iter)
	{
		(*iter)->NotifyColourPresetsChanged(this);
	}
}

void GameModel::notifyColourActivePresetChanged()
{
	for (std::vector<GameView*>::iterator iter = observers.begin(); iter != observers.end(); ++iter)
	{
		(*iter)->NotifyColourActivePresetChanged(this);
	}
}

void GameModel::notifyColourSelectorColourChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyColourSelectorColourChanged(this);
	}
}

void GameModel::notifyColourSelectorVisibilityChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyColourSelectorVisibilityChanged(this);
	}
}

void GameModel::notifyRendererChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyRendererChanged(this);
	}
}

void GameModel::notifySaveChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifySaveChanged(this);
	}
}

void GameModel::notifySimulationChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifySimulationChanged(this);
	}
}

void GameModel::notifyPausedChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyPausedChanged(this);
	}
}

void GameModel::notifyDecorationChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		//observers[i]->NotifyPausedChanged(this);
	}
}

void GameModel::notifyBrushChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyBrushChanged(this);
	}
}

void GameModel::notifyMenuListChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyMenuListChanged(this);
	}
}

void GameModel::notifyToolListChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyToolListChanged(this);
	}
}

void GameModel::notifyActiveToolsChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyActiveToolsChanged(this);
	}
}

void GameModel::notifyUserChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyUserChanged(this);
	}
}

void GameModel::notifyZoomChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyZoomChanged(this);
	}
}

void GameModel::notifyPlaceSaveChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyPlaceSaveChanged(this);
	}
}

void GameModel::notifyTransformedPlaceSaveChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyTransformedPlaceSaveChanged(this);
	}
}

void GameModel::notifyLogChanged(String entry)
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyLogChanged(this, entry);
	}
}

void GameModel::notifyInfoTipChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyInfoTipChanged(this);
	}
}

void GameModel::notifyToolTipChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyToolTipChanged(this);
	}
}

void GameModel::notifyQuickOptionsChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyQuickOptionsChanged(this);
	}
}

void GameModel::notifyLastToolChanged()
{
	for (size_t i = 0; i < observers.size(); i++)
	{
		observers[i]->NotifyLastToolChanged(this);
	}
}

bool GameModel::GetMouseClickRequired()
{
	return mouseClickRequired;
}

void GameModel::SetMouseClickRequired(bool mouseClickRequired)
{
	this->mouseClickRequired = mouseClickRequired;
}

bool GameModel::GetIncludePressure()
{
	return includePressure;
}

void GameModel::SetIncludePressure(bool includePressure)
{
	this->includePressure = includePressure;
}

void GameModel::SetPerfectCircle(bool perfectCircle)
{
	if (perfectCircle != this->perfectCircle)
	{
		this->perfectCircle = perfectCircle;
		BuildBrushList();
	}
}

bool GameModel::RemoveCustomGOLType(const ByteString &identifier)
{
	bool removedAny = false;
	auto &prefs = GlobalPrefs::Ref();
	auto customGOLTypes = prefs.Get("CustomGOL.Types", std::vector<ByteString>{});
	std::vector<ByteString> newCustomGOLTypes;
	for (auto gol : customGOLTypes)
	{
		auto parts = gol.PartitionBy(' ');
		if (parts.size() && "DEFAULT_PT_LIFECUST_" + parts[0] == identifier)
			removedAny = true;
		else
			newCustomGOLTypes.push_back(gol);
	}
	if (removedAny)
	{
		prefs.Set("CustomGOL.Types", newCustomGOLTypes);
	}
	BuildMenus();
	return removedAny;
}

void GameModel::UpdateUpTo(int upTo)
{
	if (upTo < sim->debug_nextToUpdate)
	{
		upTo = NPART;
	}
	if (sim->debug_nextToUpdate == 0)
	{
		BeforeSim();
	}
	sim->UpdateParticles(sim->debug_nextToUpdate, upTo);
	if (upTo < NPART)
	{
		sim->debug_nextToUpdate = upTo;
	}
	else
	{
		AfterSim();
		sim->debug_nextToUpdate = 0;
	}
}

void GameModel::BeforeSim()
{
	if (!sim->sys_pause || sim->framerender)
	{
		CommandInterface::Ref().HandleEvent(BeforeSimEvent{});
	}
	sim->BeforeSim();
}

void GameModel::AfterSim()
{
	sim->AfterSim();
	CommandInterface::Ref().HandleEvent(AfterSimEvent{});
}
