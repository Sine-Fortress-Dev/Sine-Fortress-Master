//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $Revision: $
// $NoKeywords: $
//
// This file contains code to allow us to associate client data with bsp leaves.
//===========================================================================//

#include "cbase.h"
#include "clientleafsystem.h"
#include "utlbidirectionalset.h"
#include "model_types.h"
#include "ivrenderview.h"
#include "tier0/vprof.h"
#include "bsptreedata.h"
#include "detailobjectsystem.h"
#include "engine/IStaticPropMgr.h"
#include "engine/ivdebugoverlay.h"
#include "vstdlib/jobthread.h"
#include "tier1/utllinkedlist.h"
#include "datacache/imdlcache.h"
#include "view.h"
#include "viewrender.h"
#include <typeinfo>
#include "con_nprint.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

class VMatrix;  // forward decl

static ConVar cl_drawleaf("cl_drawleaf", "-1", FCVAR_CHEAT );
static ConVar r_PortalTestEnts( "r_PortalTestEnts", "1", FCVAR_CHEAT, "Clip entities against portal frustums." );
static ConVar r_portalsopenall( "r_portalsopenall", "0", FCVAR_CHEAT, "Open all portals" );
static ConVar cl_threaded_client_leaf_system("cl_threaded_client_leaf_system", "0"  );

static ConVar cl_leafsystemvis("cl_leafsystemvis", "0", FCVAR_CHEAT);


DEFINE_FIXEDSIZE_ALLOCATOR( CClientRenderablesList, 1, CUtlMemoryPool::GROW_SLOW );

//-----------------------------------------------------------------------------
// Threading helpers
//-----------------------------------------------------------------------------

static void FrameLock()
{
	mdlcache->BeginLock();
}

static void FrameUnlock()
{
	mdlcache->EndLock();
}

static void CallComputeFXBlend( IClientRenderable *&pRenderable )
{
	pRenderable->ComputeFxBlend();
}

//-----------------------------------------------------------------------------
// The client leaf system
//-----------------------------------------------------------------------------
class CClientLeafSystem : public IClientLeafSystem, public ISpatialLeafEnumerator
{
public:
	virtual char const *Name() { return "CClientLeafSystem"; }

	// constructor, destructor
	CClientLeafSystem();
	virtual ~CClientLeafSystem();

	// Methods of IClientSystem
	bool Init() { return true; }
	void PostInit() {}
	void Shutdown() {}

	virtual bool IsPerFrame() { return true; }

	void PreRender();
	void PostRender() { }
	void Update(float frametime) { m_nDebugIndex = 0; }

	void LevelInitPreEntity();
	void LevelInitPostEntity() {}
	void LevelShutdownPreEntity();
	void LevelShutdownPostEntity();

	virtual void OnSave() {}
	virtual void OnRestore() {}
	virtual void SafeRemoveIfDesired() {}

// Methods of IClientLeafSystem
public:
	
	virtual void AddRenderable( IClientRenderable* pRenderable, RenderGroup_t group );
	virtual bool IsRenderableInPVS( IClientRenderable *pRenderable );
	virtual void CreateRenderableHandle( IClientRenderable* pRenderable, bool bIsStaticProp );
	virtual void RemoveRenderable( ClientRenderHandle_t handle );


	virtual void SetSubSystemDataInLeaf( int leaf, int nSubSystemIdx, CClientLeafSubSystemData *pData );
	virtual CClientLeafSubSystemData *GetSubSystemDataInLeaf( int leaf, int nSubSystemIdx );

	// FIXME: There's an incestuous relationship between DetailObjectSystem
	// and the ClientLeafSystem. Maybe they should be the same system?
	virtual void GetDetailObjectsInLeaf( int leaf, int& firstDetailObject, int& detailObjectCount );
 	virtual void SetDetailObjectsInLeaf( int leaf, int firstDetailObject, int detailObjectCount );
	virtual void DrawDetailObjectsInLeaf( int leaf, int frameNumber, int& nFirstDetailObject, int& nDetailObjectCount );
	virtual bool ShouldDrawDetailObjectsInLeaf( int leaf, int frameNumber );
	virtual void RenderableChanged( ClientRenderHandle_t handle );
	virtual void SetRenderGroup( ClientRenderHandle_t handle, RenderGroup_t group );
	virtual void ComputeTranslucentRenderLeaf( int count, const LeafIndex_t *pLeafList, const LeafFogVolume_t *pLeafFogVolumeList, int frameNumber, int viewID );
	virtual void CollateViewModelRenderables( CUtlVector< IClientRenderable * >& opaque, CUtlVector< IClientRenderable * >& translucent );
	virtual void BuildRenderablesList( const SetupRenderInfo_t &info );
			void CollateRenderablesInLeaf( int leaf, int worldListLeafIndex, const SetupRenderInfo_t &info );
	virtual void DrawStaticProps( bool enable );
	virtual void DrawSmallEntities( bool enable );
	virtual void EnableAlternateSorting( ClientRenderHandle_t handle, bool bEnable );
	virtual void EnableBloatedBounds(ClientRenderHandle_t handle, bool bEnable);

	// Adds a renderable to a set of leaves
	virtual void AddRenderableToLeaves( ClientRenderHandle_t handle, int nLeafCount, unsigned short* pLeaves );
	void AddRenderableToLeaves(ClientRenderHandle_t handle, int nLeafCount, int *pLeaves, bool bReceiveShadows);

	// The following methods are related to shadows...
	virtual ClientLeafShadowHandle_t AddShadow( ClientShadowHandle_t userId, unsigned short flags );
	virtual void RemoveShadow( ClientLeafShadowHandle_t h );

	virtual void ProjectShadow( ClientLeafShadowHandle_t handle, int nLeafCount, const int *pLeafList );
	virtual void ProjectFlashlight( ClientLeafShadowHandle_t handle, int nLeafCount, const int *pLeafList );

	// Find all shadow casters in a set of leaves
	virtual void EnumerateShadowsInLeaves( int leafCount, LeafIndex_t* pLeaves, IClientLeafShadowEnum* pEnum );
	virtual void RecomputeRenderableLeaves();
	virtual void DisableLeafReinsertion(bool bDisable);
	virtual void ComputeAllBounds(void);

	// methods of ISpatialLeafEnumerator
public:

	bool EnumerateLeaf( int leaf, intp context );

	// Adds a shadow to a leaf
	void AddShadowToLeaf( int leaf, ClientLeafShadowHandle_t handle );

	// Fill in a list of the leaves this renderable is in.
	// Returns -1 if the handle is invalid.
	int GetRenderableLeaves( ClientRenderHandle_t handle, int leaves[1024] );

	// Get leaves this renderable is in
	virtual bool GetRenderableLeaf ( ClientRenderHandle_t handle, int* pOutLeaf, const int* pInIterator = 0, int* pOutIterator = 0 );

	// Singleton instance...
	static CClientLeafSystem s_ClientLeafSystem;

private:

	enum
	{
		RENDER_FLAGS_TWOPASS = 0x01,
		RENDER_FLAGS_STATIC_PROP = 0x02,
		RENDER_FLAGS_BRUSH_MODEL = 0x04,
		RENDER_FLAGS_STUDIO_MODEL = 0x08,
		RENDER_FLAGS_HASCHANGED = 0x10,
		RENDER_FLAGS_ALTERNATE_SORTING = 0x20,
		RENDER_FLAGS_BLOAT_BOUNDS = 0x40,
		RENDER_FLAGS_BOUNDS_VALID = 0x80,
		RENDER_FLAGS_DISABLE_RENDERING = 0x100,
	};

	// All the information associated with a particular handle
	struct RenderableInfo_t
	{
		IClientRenderable* m_pRenderable;
		int					m_RenderFrame;	// which frame did I render it in?
		int					m_RenderFrame2;
		int					m_EnumCount;	// Have I been added to a particular shadow yet?
		int					m_TranslucencyCalculated;
		unsigned int		m_LeafList;		// What leafs is it in?
		unsigned int		m_RenderLeaf;	// What leaf do I render in?
		unsigned char		m_Flags;		// rendering flags
		unsigned char		m_RenderGroup;	// RenderGroup_t type
		unsigned short		m_FirstShadow;	// The first shadow caster that cast on it
		int m_Area;	// -1 if the renderable spans multiple areas.
		signed char			m_TranslucencyCalculatedView;
		Vector				m_vecBloatedAbsMins;		// Use this for tree insertion
		Vector				m_vecBloatedAbsMaxs;
		Vector				m_vecPendingBloatedAbsMins;		// This is the newly computed bloated bounds ready for comparison/update
		Vector				m_vecPendingBloatedAbsMaxs;
		Vector				m_vecAbsMins;
		Vector				m_vecAbsMaxs;
	};

	// Creates a new renderable
	void NewRenderable( IClientRenderable* pRenderable, RenderGroup_t type, int flags = 0 );

	// Adds a renderable to the list of renderables
	void AddRenderableToLeaf(int leaf, ClientRenderHandle_t handle, bool bReceiveShadows);

	void SortEntities(  const Vector &vecRenderOrigin, const Vector &vecRenderForward, CClientRenderablesList::CEntry *pEntities, int nEntities );

	virtual void ComputeBounds(RenderableInfo_t*& info);

	// remove renderables from leaves
	void InsertIntoTree(ClientRenderHandle_t& handle, const Vector& absMins, const Vector& absMaxs);
	void InsertIntoTree(ClientRenderHandle_t& handle);
	void RemoveFromTree(ClientRenderHandle_t& handle);

	// Returns if it's a view model render group
	inline bool IsViewModelRenderGroup( RenderGroup_t group ) const;

	// Adds, removes renderables from view model list
	void AddToViewModelList( ClientRenderHandle_t handle );
	void RemoveFromViewModelList( ClientRenderHandle_t handle );

	// Insert translucent renderables into list of translucent objects
	void InsertTranslucentRenderable( IClientRenderable* pRenderable,
		int& count, IClientRenderable** pList, float* pDist );

	// Used to change renderables from translucent to opaque
	// Only really used by the static prop fading...
	void ChangeRenderableRenderGroup( ClientRenderHandle_t handle, RenderGroup_t group );

	// Adds a shadow to a leaf/removes shadow from renderable
	void AddShadowToRenderable( ClientRenderHandle_t renderHandle, ClientLeafShadowHandle_t shadowHandle );
	void RemoveShadowFromRenderables( ClientLeafShadowHandle_t handle );

	// Adds a shadow to a leaf/removes shadow from renderable
	bool ShouldRenderableReceiveShadow( ClientRenderHandle_t renderHandle, int nShadowFlags );

	// Adds a shadow to a leaf/removes shadow from leaf
	void RemoveShadowFromLeaves( ClientLeafShadowHandle_t handle );

	void ProcessDirtyRenderable(ClientRenderHandle_t& handle);

	void CalcRenderableWorldSpaceAABB_Bloated(RenderableInfo_t& info, Vector& absMin, Vector& absMax);

	// Methods associated with the various bi-directional sets
	static unsigned int& FirstRenderableInLeaf( int leaf ) 
	{ 
		return s_ClientLeafSystem.m_Leaf[leaf].m_FirstElement;
	}

	static unsigned int& FirstLeafInRenderable( unsigned short renderable ) 
	{ 
		return s_ClientLeafSystem.m_Renderables[renderable].m_LeafList;
	}

	static unsigned short& FirstShadowInLeaf( int leaf ) 
	{ 
		return s_ClientLeafSystem.m_Leaf[leaf].m_FirstShadow;
	}

	static unsigned short& FirstLeafInShadow( ClientLeafShadowHandle_t shadow ) 
	{ 
		return s_ClientLeafSystem.m_Shadows[shadow].m_FirstLeaf;
	}

	static unsigned short& FirstShadowOnRenderable( unsigned short renderable ) 
	{ 
		return s_ClientLeafSystem.m_Renderables[renderable].m_FirstShadow;
	}

	static unsigned short& FirstRenderableInShadow( ClientLeafShadowHandle_t shadow ) 
	{ 
		return s_ClientLeafSystem.m_Shadows[shadow].m_FirstRenderable;
	}

	void FrameLock()
	{
		mdlcache->BeginLock();
	}

	void FrameUnlock()
	{
		mdlcache->EndLock();
	}

private:
	// The leaf contains an index into a list of renderables
	struct ClientLeaf_t
	{
		unsigned int	m_FirstElement;
		unsigned short	m_FirstShadow;

		unsigned short	m_FirstDetailProp;
		unsigned short	m_DetailPropCount;
		int				m_DetailPropRenderFrame;
		CClientLeafSubSystemData *m_pSubSystemData[N_CLSUBSYSTEMS];

	};

	// Shadow information
	struct ShadowInfo_t
	{
		unsigned short	m_FirstLeaf;
		unsigned short	m_FirstRenderable;
		int				m_EnumCount;
		ClientShadowHandle_t	m_Shadow;
		unsigned short	m_Flags;
	};

	struct EnumResult_t
	{
		int leaf;
		EnumResult_t *pNext;
	};

	struct EnumResultList_t
	{
		EnumResult_t *pHead;
		ClientRenderHandle_t handle;
	};

	// Stores data associated with each leaf.
	CUtlVector< ClientLeaf_t >	m_Leaf;

	// Stores all unique non-detail renderables
	CUtlLinkedList< RenderableInfo_t, ClientRenderHandle_t, false, unsigned int >	m_Renderables;

	// Information associated with shadows registered with the client leaf system
	CUtlLinkedList< ShadowInfo_t, ClientLeafShadowHandle_t, false, unsigned int >	m_Shadows;

	// Maintains the list of all renderables in a particular leaf
	CBidirectionalSet< int, ClientRenderHandle_t, unsigned int, unsigned int >	m_RenderablesInLeaf;

	// Maintains a list of all shadows in a particular leaf 
	CBidirectionalSet< int, ClientLeafShadowHandle_t, unsigned short, unsigned int >	m_ShadowsInLeaf;

	// Maintains a list of all shadows cast on a particular renderable
	CBidirectionalSet< ClientRenderHandle_t, ClientLeafShadowHandle_t, unsigned short, unsigned int >	m_ShadowsOnRenderable;

	// Dirty list of renderables
	CUtlVector< ClientRenderHandle_t >	m_DirtyRenderables;

	// List of renderables in view model render groups
	CUtlVector< ClientRenderHandle_t >	m_ViewModels;

	// Should I draw static props?
	bool m_DrawStaticProps;
	bool m_DrawSmallObjects;
	bool m_bDisableLeafReinsertion;

	// A little enumerator to help us when adding shadows to renderables
	int	m_ShadowEnum;

	CTSList<EnumResultList_t> m_DeferredInserts;

	int m_nDebugIndex;

	CThreadFastMutex m_DirtyRenderablesMutex;
};


//-----------------------------------------------------------------------------
// Expose IClientLeafSystem to the client dll.
//-----------------------------------------------------------------------------
CClientLeafSystem CClientLeafSystem::s_ClientLeafSystem;
IClientLeafSystem *g_pClientLeafSystem = &CClientLeafSystem::s_ClientLeafSystem;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CClientLeafSystem, IClientLeafSystem, CLIENTLEAFSYSTEM_INTERFACE_VERSION, CClientLeafSystem::s_ClientLeafSystem );

void CalcRenderableWorldSpaceAABB_Fast( IClientRenderable *pRenderable, Vector &absMin, Vector &absMax );

//-----------------------------------------------------------------------------
// Helper functions.
//-----------------------------------------------------------------------------
void DefaultRenderBoundsWorldspace( IClientRenderable *pRenderable, Vector &absMins, Vector &absMaxs )
{
	// Tracker 37433: This fixes a bug where if the stunstick is being wielded by a combine soldier, the fact that the stick was
	// attached to the soldier's hand would move it such that it would get frustum culled near the edge of the screen.
	C_BaseEntity* pEnt = pRenderable->GetIClientUnknown()->GetBaseEntity();
	C_BaseEntity* pParent;
	if (pEnt && (pParent = pEnt->GetMoveParent()) != NULL && (pEnt->GetParentAttachment() > 0 || pEnt->IsFollowingEntity()))
	{
		// Get the parent's abs space world bounds.
		CalcRenderableWorldSpaceAABB_Fast(pParent, absMins, absMaxs);

		// Add the maximum of our local render bounds. This is making the assumption that we can be at any
		// point and at any angle within the parent's world space bounds.
		Vector vAddMins, vAddMaxs;
		pEnt->GetRenderBounds(vAddMins, vAddMaxs);
		// if our origin is actually farther away than that, expand again
		float radius = pEnt->GetLocalOrigin().LengthSqr();

		float flBloatSize = MAX(vAddMins.LengthSqr(), vAddMaxs.LengthSqr());
		flBloatSize = MAX(flBloatSize, radius);
		flBloatSize = FastSqrt(flBloatSize);
		absMins -= Vector(flBloatSize, flBloatSize, flBloatSize);
		absMaxs += Vector(flBloatSize, flBloatSize, flBloatSize);
		return;
	}

	Vector mins, maxs;
	pRenderable->GetRenderBounds(mins, maxs);

	// FIXME: Should I just use a sphere here?
	// Another option is to pass the OBB down the tree; makes for a better fit
	// Generate a world-aligned AABB
	const QAngle& angles = pRenderable->GetRenderAngles();
	if (angles == vec3_angle)
	{
		const Vector& origin = pRenderable->GetRenderOrigin();
		VectorAdd(mins, origin, absMins);
		VectorAdd(maxs, origin, absMaxs);
	}
	else
	{
		TransformAABB(pRenderable->RenderableToWorldTransform(), mins, maxs, absMins, absMaxs);
	}
	Assert(absMins.IsValid() && absMaxs.IsValid());
}

// Figure out a world space bounding box that encloses the entity's local render bounds in world space.
inline void CalcRenderableWorldSpaceAABB( 
	IClientRenderable *pRenderable, 
	Vector &absMins,
	Vector &absMaxs )
{
	if (!pRenderable)
	{
		AssertMsg(false, "Cannot calculate WorldSpaceAABB for NULL renderable!\n");
		return;
	}

	pRenderable->GetRenderBoundsWorldspace(absMins, absMaxs);
}


// This gets an AABB for the renderable, but it doesn't cause a parent's bones to be setup.
// This is used for placement in the leaves, but the more expensive version is used for culling.
void CalcRenderableWorldSpaceAABB_Fast( IClientRenderable *pRenderable, Vector &absMin, Vector &absMax )
{
	C_BaseEntity *pEnt = pRenderable->GetIClientUnknown()->GetBaseEntity();
	if ( pEnt && pEnt->IsFollowingEntity() )
	{
		C_BaseEntity *pParent = pEnt->GetMoveParent();
		Assert( pParent );

		// Get the parent's abs space world bounds.
		CalcRenderableWorldSpaceAABB_Fast( pParent, absMin, absMax );

		// Add the maximum of our local render bounds. This is making the assumption that we can be at any
		// point and at any angle within the parent's world space bounds.
		Vector vAddMins, vAddMaxs;
		pEnt->GetRenderBounds( vAddMins, vAddMaxs );
		// if our origin is actually farther away than that, expand again
		float radius = pEnt->GetLocalOrigin().Length();

		float flBloatSize = MAX( vAddMins.Length(), vAddMaxs.Length() );
		flBloatSize = MAX(flBloatSize, radius);
		absMin -= Vector( flBloatSize, flBloatSize, flBloatSize );
		absMax += Vector( flBloatSize, flBloatSize, flBloatSize );
	}
	else
	{
		// Start out with our own render bounds. Since we don't have a parent, this won't incur any nasty 
		CalcRenderableWorldSpaceAABB( pRenderable, absMin, absMax );
	}
}


//-----------------------------------------------------------------------------
// constructor, destructor
//-----------------------------------------------------------------------------
CClientLeafSystem::CClientLeafSystem() : m_DrawStaticProps(true), m_DrawSmallObjects(true)
{
	// Set up the bi-directional lists...
	m_RenderablesInLeaf.Init( FirstRenderableInLeaf, FirstLeafInRenderable );
	m_ShadowsInLeaf.Init( FirstShadowInLeaf, FirstLeafInShadow ); 
	m_ShadowsOnRenderable.Init( FirstShadowOnRenderable, FirstRenderableInShadow );
	m_bDisableLeafReinsertion = false;
}

CClientLeafSystem::~CClientLeafSystem()
{
}

//-----------------------------------------------------------------------------
// Activate, deactivate static props
//-----------------------------------------------------------------------------
void CClientLeafSystem::DrawStaticProps( bool enable )
{
	m_DrawStaticProps = enable;
}

void CClientLeafSystem::DrawSmallEntities( bool enable )
{
	m_DrawSmallObjects = enable;
}

void CClientLeafSystem::DisableLeafReinsertion(bool bDisable)
{
	m_bDisableLeafReinsertion = bDisable;
}

void CClientLeafSystem::ComputeAllBounds(void)
{
	MDLCACHE_CRITICAL_SECTION();
	static CUtlVector<RenderableInfo_t*> renderablesToUpdate;
	bool bThreaded = (cl_threaded_client_leaf_system.GetBool() && g_pThreadPool->NumIdleThreads());
	for (int i = m_Renderables.Head(); i != m_Renderables.InvalidIndex(); i = m_Renderables.Next(i))
	{
		RenderableInfo_t* pInfo = &m_Renderables[i];

		if (bThreaded)
		{
			if (pInfo->m_Flags & RENDER_FLAGS_DISABLE_RENDERING)
				continue;

			if ((pInfo->m_Flags & RENDER_FLAGS_BOUNDS_VALID) == 0)
			{
				renderablesToUpdate.AddToTail(pInfo);
			}
		}
		else
		{
			ComputeBounds(pInfo);
		}
	}

	if (bThreaded)
	{
		ParallelProcess("CClientLeafSystem::ComputeAllBounds", renderablesToUpdate.Base(), renderablesToUpdate.Count(), this, &CClientLeafSystem::ComputeBounds);
		renderablesToUpdate.RemoveAll();
	}
}

//-----------------------------------------------------------------------------
// Level init, shutdown
//-----------------------------------------------------------------------------
void CClientLeafSystem::LevelInitPreEntity()
{
	MEM_ALLOC_CREDIT();

	m_Renderables.EnsureCapacity( 1024 );
	m_RenderablesInLeaf.EnsureCapacity( 1024 );
	m_ShadowsInLeaf.EnsureCapacity( 256 );
	m_ShadowsOnRenderable.EnsureCapacity( 256 );
	m_DirtyRenderables.EnsureCapacity( 256 );

	// Add all the leaves we'll need
	int leafCount = engine->LevelLeafCount();
	m_Leaf.EnsureCapacity( leafCount );

	ClientLeaf_t newLeaf;
	newLeaf.m_FirstElement = m_RenderablesInLeaf.InvalidIndex();
	newLeaf.m_FirstShadow = m_ShadowsInLeaf.InvalidIndex();
	memset( newLeaf.m_pSubSystemData, 0, sizeof( newLeaf.m_pSubSystemData ) );
	newLeaf.m_FirstDetailProp = 0;
	newLeaf.m_DetailPropCount = 0;
	newLeaf.m_DetailPropRenderFrame = -1;
	while ( --leafCount >= 0 )
	{
		m_Leaf.AddToTail( newLeaf );
	}
}

void CClientLeafSystem::LevelShutdownPreEntity()
{
}

void CClientLeafSystem::LevelShutdownPostEntity()
{
	AUTO_LOCK(m_DirtyRenderablesMutex);
	m_ViewModels.Purge();
	m_Renderables.Purge();
	m_RenderablesInLeaf.Purge();
	m_Shadows.Purge();

	// delete subsystem data
	for( int i = 0; i < m_Leaf.Count() ; i++ )
	{
		for( int j = 0 ; j < ARRAYSIZE( m_Leaf[i].m_pSubSystemData ) ; j++ )
		{
			if ( m_Leaf[i].m_pSubSystemData[j] )
			{
				delete m_Leaf[i].m_pSubSystemData[j];
				m_Leaf[i].m_pSubSystemData[j] = NULL;
			}
		}
	}
	m_Leaf.Purge();
	m_ShadowsInLeaf.Purge();
	m_ShadowsOnRenderable.Purge();
	m_DirtyRenderables.Purge();
}


//-----------------------------------------------------------------------------
// This is what happens before rendering a particular view
//-----------------------------------------------------------------------------
void CClientLeafSystem::PreRender()
{
	// FIXME: This should never need to happen here!
	// At the moment, it's necessary because of the horrid viewmodel/combatweapon
	// confusion in the code where a combat weapon changes its rendering model
	// per view.
	//RecomputeRenderableLeaves();
}

// Use this to make sure we're not adding the same renderables to the list while we're going through and re-inserting them into the clientleafsystem
static bool s_bIsInRecomputeRenderableLeaves = false;

void CClientLeafSystem::RecomputeRenderableLeaves()
{
	VPROF_BUDGET("CClientLeafSystem::RecomputeRenderableLeaves", "RecomputeRenderableLeaves");

	AUTO_LOCK(m_DirtyRenderablesMutex);

	int i;
	int nIterations = 0;

	const bool bDebugLeafSystem = !IsGameConsole() && cl_leafsystemvis.GetBool();

	while (m_DirtyRenderables.Count())
	{
		if (++nIterations > 10)
		{
			Warning("Too many dirty renderables!\n");
			break;
		}

		s_bIsInRecomputeRenderableLeaves = true;

		int nDirty = m_DirtyRenderables.Count();

		bool bThreaded = (cl_threaded_client_leaf_system.GetBool() && g_pThreadPool->NumIdleThreads());

		if (!bThreaded)
		{
			for (i = nDirty; --i >= 0; )
			{
				ClientRenderHandle_t handle = m_DirtyRenderables[i];
				ProcessDirtyRenderable(handle);
			}
		}
		else
		{
			for (i = nDirty; --i >= 0; )
			{
				ClientRenderHandle_t handle = m_DirtyRenderables[i];
				RenderableInfo_t& info = m_Renderables[handle];

				Assert(info.m_Flags & RENDER_FLAGS_HASCHANGED);

				// See note at the end of RecomputeRenderableLeaves
				info.m_Flags &= ~RENDER_FLAGS_HASCHANGED;

				if (info.m_vecPendingBloatedAbsMins != info.m_vecBloatedAbsMins || info.m_vecPendingBloatedAbsMaxs != info.m_vecBloatedAbsMaxs)
				{
					// Update position in leaf system
					RemoveFromTree(handle);
					if (bDebugLeafSystem)
					{
						debugoverlay->AddBoxOverlay(vec3_origin, info.m_vecPendingBloatedAbsMins, info.m_vecPendingBloatedAbsMaxs, QAngle(0, 0, 0), 0, 255, 0, 0, 0.0f);
					}
					info.m_vecBloatedAbsMins = info.m_vecPendingBloatedAbsMins;
					info.m_vecBloatedAbsMaxs = info.m_vecPendingBloatedAbsMaxs;
				}
				else
				{
					// We don't need to update it
					m_DirtyRenderables.Remove(i);
				}
			}

			nDirty = m_DirtyRenderables.Count();

			if (nDirty)
			{
				// InsertIntoTree can result in new renderables being added, so copy:
				ClientRenderHandle_t* pDirtyRenderables = (ClientRenderHandle_t*)alloca(sizeof(ClientRenderHandle_t) * nDirty);
				memcpy(pDirtyRenderables, m_DirtyRenderables.Base(), sizeof(ClientRenderHandle_t) * nDirty);
				ParallelProcess("CClientLeafSystem::RecomputeRenderableLeaves", pDirtyRenderables, nDirty, this, &CClientLeafSystem::InsertIntoTree);
			}
		}

		if (m_DeferredInserts.Count())
		{
			EnumResultList_t enumResultList;
			while (m_DeferredInserts.PopItem(&enumResultList))
			{
				m_ShadowEnum++;
				const bool bReceiveShadows = ShouldRenderableReceiveShadow(enumResultList.handle, SHADOW_FLAGS_PROJECTED_TEXTURE_TYPE_MASK);
				while (enumResultList.pHead)
				{
					EnumResult_t* p = enumResultList.pHead;
					enumResultList.pHead = p->pNext;
					AddRenderableToLeaf(p->leaf, enumResultList.handle, bReceiveShadows);
					delete p;
				}
			}
		}

		s_bIsInRecomputeRenderableLeaves = false;

		// NOTE: If we get the following error displayed in the console spew
		//       "Re-entrancy found in CClientLeafSystem::RenderableChanged\n"
		//		 We'll have to reenable this code and remove the line that
		//		 removes the RENDER_FLAGS_HASCHANGED in the loop above.
#if 0
		for (i = nDirty; --i >= 0; )
		{
			ClientRenderHandle_t handle = m_DirtyRenderables[i];
			RenderableInfo_t& renderable = m_Renderables[handle];

			renderable.m_Flags &= ~RENDER_FLAGS_HASCHANGED;
		}
#endif

		m_DirtyRenderables.RemoveMultiple(0, nDirty);
	}
}


//-----------------------------------------------------------------------------
// Creates a new renderable
//-----------------------------------------------------------------------------
void CClientLeafSystem::NewRenderable( IClientRenderable* pRenderable, RenderGroup_t type, int flags )
{
	Assert( pRenderable );
	Assert( pRenderable->RenderHandle() == INVALID_CLIENT_RENDER_HANDLE );

	ClientRenderHandle_t handle = m_Renderables.AddToTail();
	RenderableInfo_t &info = m_Renderables[handle];

	// We need to know if it's a brush model for shadows
	int modelType = modelinfo->GetModelType( pRenderable->GetModel() );
	if (modelType == mod_brush)
	{
		flags |= RENDER_FLAGS_BRUSH_MODEL;
	}
	else if ( modelType == mod_studio )
	{
		flags |= RENDER_FLAGS_STUDIO_MODEL;
	}

	info.m_Area = -1;
	info.m_pRenderable = pRenderable;
	info.m_RenderFrame = -1;
	info.m_RenderFrame2 = -1;
	info.m_TranslucencyCalculated = -1;
	info.m_TranslucencyCalculatedView = VIEW_ILLEGAL;
	info.m_FirstShadow = m_ShadowsOnRenderable.InvalidIndex();
	info.m_LeafList = m_RenderablesInLeaf.InvalidIndex();
	info.m_Flags = flags;
	info.m_RenderGroup = (unsigned char)type;
	info.m_EnumCount = 0;
	info.m_RenderLeaf = m_RenderablesInLeaf.InvalidIndex();
	info.m_vecBloatedAbsMins.Init(FLT_MAX, FLT_MAX, FLT_MAX);
	info.m_vecBloatedAbsMaxs.Init(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	info.m_vecPendingBloatedAbsMins.Init();
	info.m_vecPendingBloatedAbsMaxs.Init();
	info.m_vecAbsMins.Init();
	info.m_vecAbsMaxs.Init();

	if ( IsViewModelRenderGroup( (RenderGroup_t)info.m_RenderGroup ) )
	{
		AddToViewModelList( handle );
	}

	pRenderable->RenderHandle() = handle;
}

void CClientLeafSystem::CreateRenderableHandle( IClientRenderable* pRenderable, bool bIsStaticProp )
{
	// FIXME: The argument is unnecessary if we could get this next line to work
	// the reason why we can't is because currently there are IClientRenderables
	// which don't correctly implement GetRefEHandle.

	//bool bIsStaticProp = staticpropmgr->IsStaticProp( pRenderable->GetIClientUnknown() );

	// Add the prop to all the leaves it lies in
	RenderGroup_t group = pRenderable->IsTransparent() ? RENDER_GROUP_TRANSLUCENT_ENTITY : RENDER_GROUP_OPAQUE_ENTITY;

	bool bTwoPass = false;
	if ( group == RENDER_GROUP_TRANSLUCENT_ENTITY )
	{
		bTwoPass = pRenderable->IsTwoPass( );
	}

	int flags = 0;
	if ( bIsStaticProp )
	{
		flags = RENDER_FLAGS_STATIC_PROP;
		if ( group == RENDER_GROUP_OPAQUE_ENTITY )
		{
			group = RENDER_GROUP_OPAQUE_STATIC;
		}
	}

	if (bTwoPass)
	{
		flags |= RENDER_FLAGS_TWOPASS;
	}

	NewRenderable( pRenderable, group, flags );

	if (bIsStaticProp)
	{
		RenderableInfo_t* pInfo = &m_Renderables[pRenderable->RenderHandle()];
		ComputeBounds(pInfo);
	}
}


//-----------------------------------------------------------------------------
// Used to change renderables from translucent to opaque
//-----------------------------------------------------------------------------
void CClientLeafSystem::ChangeRenderableRenderGroup( ClientRenderHandle_t handle, RenderGroup_t group )
{
	RenderableInfo_t &info = m_Renderables[handle];
	info.m_RenderGroup = (unsigned char)group;
}


//-----------------------------------------------------------------------------
// Use alternate translucent sorting algorithm (draw translucent objects in the furthest leaf they lie in)
//-----------------------------------------------------------------------------
void CClientLeafSystem::EnableAlternateSorting( ClientRenderHandle_t handle, bool bEnable )
{
	RenderableInfo_t &info = m_Renderables[handle];
	if ( bEnable )
	{
		info.m_Flags |= RENDER_FLAGS_ALTERNATE_SORTING;
	}
	else
	{
		info.m_Flags &= ~RENDER_FLAGS_ALTERNATE_SORTING; 
	}
}

void CClientLeafSystem::EnableBloatedBounds(ClientRenderHandle_t handle, bool bEnable)
{
	if (handle == INVALID_CLIENT_RENDER_HANDLE)
		return;

	RenderableInfo_t& info = m_Renderables[handle];
	if (bEnable)
	{
		info.m_Flags |= RENDER_FLAGS_BLOAT_BOUNDS;
	}
	else
	{
		if (info.m_Flags & RENDER_FLAGS_BLOAT_BOUNDS)
		{
			info.m_Flags &= ~RENDER_FLAGS_BLOAT_BOUNDS;

			// Necessary to generate unbloated bounds later
			RenderableChanged(handle);
		}
	}
}

//-----------------------------------------------------------------------------
// Add/remove renderable
//-----------------------------------------------------------------------------
void CClientLeafSystem::AddRenderable( IClientRenderable* pRenderable, RenderGroup_t group )
{
	// force a relink we we try to draw it for the first time
	int flags = RENDER_FLAGS_HASCHANGED;

	if ( group == RENDER_GROUP_TWOPASS )
	{
		group = RENDER_GROUP_TRANSLUCENT_ENTITY;
		flags |= RENDER_FLAGS_TWOPASS;
	}

	NewRenderable( pRenderable, group, flags );
	ClientRenderHandle_t handle = pRenderable->RenderHandle();
	m_DirtyRenderables.AddToTail( handle );
}

void CClientLeafSystem::RemoveRenderable( ClientRenderHandle_t handle )
{
	// This can happen upon level shutdown
	if (!m_Renderables.IsValidIndex(handle))
		return;

	// Reset the render handle in the entity.
	IClientRenderable *pRenderable = m_Renderables[handle].m_pRenderable;
	Assert( handle == pRenderable->RenderHandle() );
	pRenderable->RenderHandle() = INVALID_CLIENT_RENDER_HANDLE;

	// Reemove the renderable from the dirty list
	if ( m_Renderables[handle].m_Flags & RENDER_FLAGS_HASCHANGED )
	{
		AUTO_LOCK(m_DirtyRenderablesMutex);
		// NOTE: This isn't particularly fast (linear search),
		// but I'm assuming it's an unusual case where we remove 
		// renderables that are changing or that m_DirtyRenderables usually
		// only has a couple entries
		int i = m_DirtyRenderables.Find( handle );
		Assert( i != m_DirtyRenderables.InvalidIndex() );
		m_DirtyRenderables.FastRemove( i ); 
	}

	if ( IsViewModelRenderGroup( (RenderGroup_t)m_Renderables[handle].m_RenderGroup ) )
	{
		RemoveFromViewModelList( handle );
	}

	RemoveFromTree( handle );
	m_Renderables.Remove( handle );
}


int CClientLeafSystem::GetRenderableLeaves( ClientRenderHandle_t handle, int leaves[1024] )
{
	if ( !m_Renderables.IsValidIndex( handle ) )
		return -1;

	RenderableInfo_t *pRenderable = &m_Renderables[handle];
	if ( pRenderable->m_LeafList == m_RenderablesInLeaf.InvalidIndex() )
		return -1;

	int nLeaves = 0;
	for ( int i=m_RenderablesInLeaf.FirstBucket( handle ); i != m_RenderablesInLeaf.InvalidIndex(); i = m_RenderablesInLeaf.NextBucket( i ) )
	{
		leaves[nLeaves++] = m_RenderablesInLeaf.Bucket( i );
		if ( nLeaves >= 1024 )
			break;
	}
	return nLeaves;
}


//-----------------------------------------------------------------------------
// Retrieve leaf handles to leaves a renderable is in
// the pOutLeaf parameter is filled with the leaf the renderable is in.
// If pInIterator is not specified, pOutLeaf is the first leaf in the list.
// if pInIterator is specified, that iterator is used to return the next leaf
// in the list in pOutLeaf.
// the pOutIterator parameter is filled with the iterater which index to the pOutLeaf returned.
//
// Returns false on failure cases where pOutLeaf will be invalid. CHECK THE RETURN!
//-----------------------------------------------------------------------------
bool CClientLeafSystem::GetRenderableLeaf(ClientRenderHandle_t handle, int* pOutLeaf, const int* pInIterator /* = 0 */, int* pOutIterator /* = 0  */)
{
	// bail on invalid handle
	if ( !m_Renderables.IsValidIndex( handle ) )
		return false;

	// bail on no output value pointer
	if ( !pOutLeaf )
		return false;

	// an iterator was specified
	if ( pInIterator )
	{
		int iter = *pInIterator;

		// test for invalid iterator
		if ( iter == m_RenderablesInLeaf.InvalidIndex() )
			return false;

		int iterNext =  m_RenderablesInLeaf.NextBucket( iter );

		// test for end of list
		if ( iterNext == m_RenderablesInLeaf.InvalidIndex() )
			return false;

		// Give the caller the iterator used
		if ( pOutIterator )
		{
			*pOutIterator = iterNext;
		}
		
		// set output value to the next leaf
		*pOutLeaf = m_RenderablesInLeaf.Bucket( iterNext );

	}
	else // no iter param, give them the first bucket in the renderable's list
	{
		int iter = m_RenderablesInLeaf.FirstBucket( handle );

		if ( iter == m_RenderablesInLeaf.InvalidIndex() )
			return false;

		// Set output value to this leaf
		*pOutLeaf = m_RenderablesInLeaf.Bucket( iter );

		// give this iterator to caller
		if ( pOutIterator )
		{
			*pOutIterator = iter;
		}
		
	}
	
	return true;
}

bool CClientLeafSystem::IsRenderableInPVS( IClientRenderable *pRenderable )
{
	ClientRenderHandle_t handle = pRenderable->RenderHandle();
	int leaves[1024];
	int nLeaves = GetRenderableLeaves( handle, leaves );
	if ( nLeaves == -1 )
		return false;

	// Ask the engine if this guy is visible.
	return render->AreAnyLeavesVisible( leaves, nLeaves );
}


void CClientLeafSystem::SetSubSystemDataInLeaf( int leaf, int nSubSystemIdx, CClientLeafSubSystemData *pData )
{
	assert( nSubSystemIdx < N_CLSUBSYSTEMS );
	if ( !m_Leaf.IsValidIndex( leaf ) )
	{
		Assert( false );
		return;
	}
	if ( m_Leaf[leaf].m_pSubSystemData[nSubSystemIdx] )
		delete m_Leaf[leaf].m_pSubSystemData[nSubSystemIdx];
	m_Leaf[leaf].m_pSubSystemData[nSubSystemIdx] = pData;
}

CClientLeafSubSystemData *CClientLeafSystem::GetSubSystemDataInLeaf( int leaf, int nSubSystemIdx )
{
	assert( nSubSystemIdx < N_CLSUBSYSTEMS );
	if ( !m_Leaf.IsValidIndex( leaf ) )
	{
		Assert( false );
		return NULL;
	}
	return m_Leaf[leaf].m_pSubSystemData[nSubSystemIdx];
}

//-----------------------------------------------------------------------------
// Indicates which leaves detail objects are in
//-----------------------------------------------------------------------------
void CClientLeafSystem::SetDetailObjectsInLeaf( int leaf, int firstDetailObject,
											    int detailObjectCount )
{
	if ( !m_Leaf.IsValidIndex( leaf ) )
	{
		Assert( false );
		return;
	}
	m_Leaf[leaf].m_FirstDetailProp = firstDetailObject;
	m_Leaf[leaf].m_DetailPropCount = detailObjectCount;
}

//-----------------------------------------------------------------------------
// Returns the detail objects in a leaf
//-----------------------------------------------------------------------------
void CClientLeafSystem::GetDetailObjectsInLeaf( int leaf, int& firstDetailObject,
											    int& detailObjectCount )
{
	if ( !m_Leaf.IsValidIndex( leaf ) )
	{
		Assert( false );
		return;
	}
	firstDetailObject = m_Leaf[leaf].m_FirstDetailProp;
	detailObjectCount = m_Leaf[leaf].m_DetailPropCount;
}


//-----------------------------------------------------------------------------
// Create/destroy shadows...
//-----------------------------------------------------------------------------
ClientLeafShadowHandle_t CClientLeafSystem::AddShadow( ClientShadowHandle_t userId, unsigned short flags )
{
	ClientLeafShadowHandle_t idx = m_Shadows.AddToTail();
	m_Shadows[idx].m_Shadow = userId;
	m_Shadows[idx].m_FirstLeaf = m_ShadowsInLeaf.InvalidIndex();
	m_Shadows[idx].m_FirstRenderable = m_ShadowsOnRenderable.InvalidIndex();
	m_Shadows[idx].m_EnumCount = 0;
	m_Shadows[idx].m_Flags = flags;
	return idx;
}

void CClientLeafSystem::RemoveShadow( ClientLeafShadowHandle_t handle )
{
	// Remove the shadow from all leaves + renderables...
	RemoveShadowFromLeaves( handle );
	RemoveShadowFromRenderables( handle );

	// Blow away the handle
	m_Shadows.Remove( handle );
}


//-----------------------------------------------------------------------------
// Adds a shadow to a leaf/removes shadow from renderable
//-----------------------------------------------------------------------------
inline bool CClientLeafSystem::ShouldRenderableReceiveShadow( ClientRenderHandle_t renderHandle, int nShadowFlags )
{
	RenderableInfo_t &renderable = m_Renderables[renderHandle];
	if( !( renderable.m_Flags & ( RENDER_FLAGS_BRUSH_MODEL | RENDER_FLAGS_STATIC_PROP | RENDER_FLAGS_STUDIO_MODEL ) ) )
		return false;

	return renderable.m_pRenderable->ShouldReceiveProjectedTextures( nShadowFlags );
}


//-----------------------------------------------------------------------------
// Adds a shadow to a leaf/removes shadow from renderable
//-----------------------------------------------------------------------------
void CClientLeafSystem::AddShadowToRenderable( ClientRenderHandle_t renderHandle, 
										ClientLeafShadowHandle_t shadowHandle )
{
	RenderableInfo_t const& info = m_Renderables[renderHandle];

	// Check if this renderable receives the type of projected texture that shadowHandle refers to.
	int nShadowFlags = m_Shadows[shadowHandle].m_Flags;
	if ( !ShouldRenderableReceiveShadow( renderHandle, nShadowFlags ) )
		return;

	m_ShadowsOnRenderable.AddElementToBucket( renderHandle, shadowHandle );

	// Also, do some stuff specific to the particular types of renderables

	// If the renderable is a brush model, then add this shadow to it
	if (info.m_Flags & RENDER_FLAGS_BRUSH_MODEL)
	{
		g_pClientShadowMgr->AddShadowToReceiver( m_Shadows[shadowHandle].m_Shadow,
			info.m_pRenderable, SHADOW_RECEIVER_BRUSH_MODEL );
	}
	else if (info.m_Flags & RENDER_FLAGS_STATIC_PROP)
	{
		g_pClientShadowMgr->AddShadowToReceiver( m_Shadows[shadowHandle].m_Shadow,
			info.m_pRenderable, SHADOW_RECEIVER_STATIC_PROP );
	}
	else if (info.m_Flags & RENDER_FLAGS_STUDIO_MODEL)
	{
		g_pClientShadowMgr->AddShadowToReceiver( m_Shadows[shadowHandle].m_Shadow,
			info.m_pRenderable, SHADOW_RECEIVER_STUDIO_MODEL );
	}
}

void CClientLeafSystem::RemoveShadowFromRenderables( ClientLeafShadowHandle_t handle )
{
	m_ShadowsOnRenderable.RemoveElement( handle );
}


//-----------------------------------------------------------------------------
// Adds a shadow to a leaf/removes shadow from leaf
//-----------------------------------------------------------------------------
void CClientLeafSystem::AddShadowToLeaf( int leaf, ClientLeafShadowHandle_t shadow )
{
	m_ShadowsInLeaf.AddElementToBucket( leaf, shadow ); 

	// Add the shadow exactly once to all renderables in the leaf
	unsigned int i = m_RenderablesInLeaf.FirstElement( leaf );
	while ( i != m_RenderablesInLeaf.InvalidIndex() )
	{
		ClientRenderHandle_t renderable = m_RenderablesInLeaf.Element(i);
		RenderableInfo_t& info = m_Renderables[renderable];

		// Add each shadow exactly once to each renderable
		if (info.m_EnumCount != m_ShadowEnum)
		{
			AddShadowToRenderable( renderable, shadow );
			info.m_EnumCount = m_ShadowEnum;
		}

		Assert( m_ShadowsInLeaf.NumAllocated() < 2000 );

		i = m_RenderablesInLeaf.NextElement(i);
	}
}

void CClientLeafSystem::RemoveShadowFromLeaves( ClientLeafShadowHandle_t handle )
{
	m_ShadowsInLeaf.RemoveElement( handle );
}

void CClientLeafSystem::ProcessDirtyRenderable(ClientRenderHandle_t& handle)
{
	RenderableInfo_t& info = m_Renderables[handle];

	Assert(m_Renderables[handle].m_Flags & RENDER_FLAGS_HASCHANGED);

	// See note at the end of RecomputeRenderableLeaves
	info.m_Flags &= ~RENDER_FLAGS_HASCHANGED;

	Vector absMins, absMaxs;
	CalcRenderableWorldSpaceAABB_Bloated(info, absMins, absMaxs);
	if (absMins != info.m_vecBloatedAbsMins || absMaxs != info.m_vecBloatedAbsMaxs)
	{
		// Update position in leaf system
		RemoveFromTree(handle);
		InsertIntoTree(handle, absMins, absMaxs);
		const bool bDebugLeafSystem = !IsGameConsole() && cl_leafsystemvis.GetBool();
		if (bDebugLeafSystem)
		{
			debugoverlay->AddBoxOverlay(vec3_origin, absMins, absMaxs, QAngle(0, 0, 0), 0, 255, 0, 0, 0);
		}
		info.m_vecBloatedAbsMins = absMins;
		info.m_vecBloatedAbsMaxs = absMaxs;
	}
}

//-----------------------------------------------------------------------------
// Computes a bloated bounding box to reduce insertions into the tree
//-----------------------------------------------------------------------------
#define BBOX_GRANULARITY 32.0f
#define MIN_SHRINK_VOLUME ( 32.0f * 32.0f * 32.0f )

void CClientLeafSystem::CalcRenderableWorldSpaceAABB_Bloated(RenderableInfo_t& info, Vector& absMin,
	Vector& absMax)
{
	if ((info.m_Flags & RENDER_FLAGS_BOUNDS_VALID) == 0)
	{
		DevWarning("Updated bounds outside of ComputeAllBounds!\n");
		CalcRenderableWorldSpaceAABB(info.m_pRenderable, absMin, absMax);
		info.m_vecAbsMins = absMin;
		info.m_vecAbsMaxs = absMax;
		info.m_Flags |= RENDER_FLAGS_BOUNDS_VALID;
	}
	else
	{
		absMin = info.m_vecAbsMins;
		absMax = info.m_vecAbsMaxs;
	}

	// Bloat bounds to avoid reinsertion into tree
	absMin.x = floor(absMin.x / BBOX_GRANULARITY) * BBOX_GRANULARITY;
	absMin.y = floor(absMin.y / BBOX_GRANULARITY) * BBOX_GRANULARITY;
	absMin.z = floor(absMin.z / BBOX_GRANULARITY) * BBOX_GRANULARITY;

	absMax.x = ceil(absMax.x / BBOX_GRANULARITY) * BBOX_GRANULARITY;
	absMax.y = ceil(absMax.y / BBOX_GRANULARITY) * BBOX_GRANULARITY;
	absMax.z = ceil(absMax.z / BBOX_GRANULARITY) * BBOX_GRANULARITY;

	// Optimization to make particle systems not re-insert themselves
	if (info.m_Flags & RENDER_FLAGS_BLOAT_BOUNDS)
	{
		Vector vecTempMin, vecTempMax;
		VectorMin(info.m_vecBloatedAbsMins, absMin, vecTempMin);
		VectorMax(info.m_vecBloatedAbsMaxs, absMax, vecTempMax);
		float flTempVolume = ComputeVolume(vecTempMin, vecTempMax);
		float flCurrVolume = ComputeVolume(absMin, absMax);

		if ((flTempVolume <= MIN_SHRINK_VOLUME) || (flCurrVolume * 2.0f >= flTempVolume))
		{
			absMin = vecTempMin;
			absMax = vecTempMax;
		}
	}
}

//-----------------------------------------------------------------------------
// Adds a shadow to all leaves listed
//-----------------------------------------------------------------------------
void CClientLeafSystem::ProjectShadow( ClientLeafShadowHandle_t handle, int nLeafCount, const int *pLeafList )
{
	// Remove the shadow from any leaves it current exists in
	RemoveShadowFromLeaves( handle );
	RemoveShadowFromRenderables( handle );

	Assert( ( m_Shadows[handle].m_Flags & SHADOW_FLAGS_PROJECTED_TEXTURE_TYPE_MASK ) == SHADOW_FLAGS_SHADOW );

	// This will help us to avoid adding the shadow multiple times to a renderable
	++m_ShadowEnum;

	for ( int i = 0; i < nLeafCount; ++i )
	{
		AddShadowToLeaf( pLeafList[i], handle );
	}
}

void CClientLeafSystem::ProjectFlashlight( ClientLeafShadowHandle_t handle, int nLeafCount, const int *pLeafList )
{
	VPROF_BUDGET( "CClientLeafSystem::ProjectFlashlight", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING );

	// Remove the shadow from any leaves it current exists in
	RemoveShadowFromLeaves( handle );
	RemoveShadowFromRenderables( handle );

	Assert( ( m_Shadows[handle].m_Flags & SHADOW_FLAGS_PROJECTED_TEXTURE_TYPE_MASK ) == SHADOW_FLAGS_FLASHLIGHT );
	
	// This will help us to avoid adding the shadow multiple times to a renderable
	++m_ShadowEnum;

	for ( int i = 0; i < nLeafCount; ++i )
	{
		AddShadowToLeaf( pLeafList[i], handle );
	}
}


//-----------------------------------------------------------------------------
// Find all shadow casters in a set of leaves
//-----------------------------------------------------------------------------
void CClientLeafSystem::EnumerateShadowsInLeaves( int leafCount, LeafIndex_t* pLeaves, IClientLeafShadowEnum* pEnum )
{
	if (leafCount == 0)
		return;

	// This will help us to avoid enumerating the shadow multiple times
	++m_ShadowEnum;

	for (int i = 0; i < leafCount; ++i)
	{
		int leaf = pLeaves[i];

		unsigned short j = m_ShadowsInLeaf.FirstElement( leaf );
		while ( j != m_ShadowsInLeaf.InvalidIndex() )
		{
			ClientLeafShadowHandle_t shadow = m_ShadowsInLeaf.Element(j);
			ShadowInfo_t& info = m_Shadows[shadow];

			if (info.m_EnumCount != m_ShadowEnum)
			{
				pEnum->EnumShadow(info.m_Shadow);
				info.m_EnumCount = m_ShadowEnum;
			}

			j = m_ShadowsInLeaf.NextElement(j);
		}
	}
}


//-----------------------------------------------------------------------------
// Adds a renderable to a leaf
//-----------------------------------------------------------------------------
void CClientLeafSystem::AddRenderableToLeaf(int leaf, ClientRenderHandle_t renderable, bool bReceiveShadows)
{
#ifdef VALIDATE_CLIENT_LEAF_SYSTEM
	m_RenderablesInLeaf.ValidateAddElementToBucket( leaf, renderable );
#endif

#ifdef DUMP_RENDERABLE_LEAFS
	static uint32 count = 0;
	if (count < m_RenderablesInLeaf.NumAllocated())
	{
		count = m_RenderablesInLeaf.NumAllocated();
		Msg("********** frame: %d count:%u ***************\n", gpGlobals->framecount, count );

		if (count >= 20000)
		{
			for (int j = 0; j < m_RenderablesInLeaf.NumAllocated(); j++)
			{
				const ClientRenderHandle_t& renderable = m_RenderablesInLeaf.Element(j);
				RenderableInfo_t& info = m_Renderables[renderable];

				char pTemp[256];
				const char *pClassName = "<unknown renderable>";
				C_BaseEntity *pEnt = info.m_pRenderable->GetIClientUnknown()->GetBaseEntity();
				if ( pEnt )
				{
					pClassName = pEnt->GetClassname();
				}
				else
				{
					CNewParticleEffect *pEffect = dynamic_cast< CNewParticleEffect*>( info.m_pRenderable );
					if ( pEffect )
					{
						Vector mins, maxs;
						pEffect->GetRenderBounds(mins, maxs);
						Q_snprintf( pTemp, sizeof(pTemp), "ps: %s %.2f,%.2f", pEffect->GetEffectName(), maxs.x - mins.x, maxs.y - mins.y );
						pClassName = pTemp;
					}
					else if ( dynamic_cast< CParticleEffectBinding* >( info.m_pRenderable ) )
					{
						pClassName = "<old particle system>";
					}
				}

				Msg(" %d: %p group:%d %s %d %d TransCalc:%d renderframe:%d\n", j, info.m_pRenderable, info.m_RenderGroup, pClassName,
					info.m_LeafList, info.m_RenderLeaf, info.m_TranslucencyCalculated, info.m_RenderFrame);
			}

			DebuggerBreak();
		}
	}
#endif // DUMP_RENDERABLE_LEAFS

	m_RenderablesInLeaf.AddElementToBucket(leaf, renderable);

	if ( !bReceiveShadows )
		return;

	// Add all shadows in the leaf to the renderable...
	unsigned short i = m_ShadowsInLeaf.FirstElement( leaf );
	while (i != m_ShadowsInLeaf.InvalidIndex() )
	{
		ClientLeafShadowHandle_t shadow = m_ShadowsInLeaf.Element(i);
		ShadowInfo_t& info = m_Shadows[shadow];

		// Add each shadow exactly once to each renderable
		if (info.m_EnumCount != m_ShadowEnum)
		{
			AddShadowToRenderable( renderable, shadow );
			info.m_EnumCount = m_ShadowEnum;
		}

		i = m_ShadowsInLeaf.NextElement(i);
	}
}

//-----------------------------------------------------------------------------
// Adds a renderable to a set of leaves
//-----------------------------------------------------------------------------
void CClientLeafSystem::AddRenderableToLeaves(ClientRenderHandle_t handle, int nLeafCount, int* pLeaves, bool bReceiveShadows)
{
	for (int j = 0; j < nLeafCount; ++j)
	{
		AddRenderableToLeaf(pLeaves[j], handle, bReceiveShadows);
	}
	m_Renderables[handle].m_Area = engine->GetLeavesArea(pLeaves, nLeafCount);
}

void CClientLeafSystem::AddRenderableToLeaves(ClientRenderHandle_t handle, int nLeafCount, unsigned short* pLeaves)
{
	// Working around the old interface to the new
	int* pNewLeaves = new int[nLeafCount];
	for (int i = 0; i < nLeafCount; i++)
	{
		pNewLeaves[i] = pLeaves[i];
	}
	const bool bReceiveShadow = ShouldRenderableReceiveShadow(handle, SHADOW_FLAGS_PROJECTED_TEXTURE_TYPE_MASK);
	AddRenderableToLeaves(handle, nLeafCount, pNewLeaves, bReceiveShadow);
	delete[] pNewLeaves;
}

//-----------------------------------------------------------------------------
// Inserts an element into the tree
//-----------------------------------------------------------------------------
bool CClientLeafSystem::EnumerateLeaf( int leaf, intp context )
{
	EnumResultList_t *pList = (EnumResultList_t *)context;
	if ( ThreadInMainThread() )
	{
		const bool bReceiveShadow = ShouldRenderableReceiveShadow(pList->handle, SHADOW_FLAGS_PROJECTED_TEXTURE_TYPE_MASK);
		AddRenderableToLeaf(leaf, pList->handle, bReceiveShadow);
	}
	else
	{
		EnumResult_t *p = new EnumResult_t;
		p->leaf = leaf;
		p->pNext = pList->pHead;
		pList->pHead = p;
	}
	return true;
}

void CClientLeafSystem::InsertIntoTree(ClientRenderHandle_t& handle, const Vector& absMins, const Vector& absMaxs)
{
	Assert(absMins.IsValid() && absMaxs.IsValid());

	// NOTE: The render bounds here are relative to the renderable's coordinate system
	RenderableInfo_t& info = m_Renderables[handle];
	info.m_vecBloatedAbsMins = absMins;
	info.m_vecBloatedAbsMaxs = absMaxs;

	InsertIntoTree(handle);
}

void CClientLeafSystem::InsertIntoTree(ClientRenderHandle_t& handle)
{
	RenderableInfo_t& info = m_Renderables[handle];

	EnumResultList_t list = { NULL, handle };

	int leafList[1024];
	ISpatialQuery* pQuery = engine->GetBSPTreeQuery();
	pQuery->EnumerateLeavesInBox(info.m_vecBloatedAbsMins, info.m_vecBloatedAbsMaxs, this, (intp)&list);

	if (cl_leafsystemvis.GetBool())
	{
		char pTemp[256];
		const char* pClassName = "<unknown renderable>";
		C_BaseEntity* pEnt = info.m_pRenderable->GetIClientUnknown()->GetBaseEntity();
		if (pEnt)
		{
			pClassName = pEnt->GetClassname();
		}
		else
		{
			CNewParticleEffect* pEffect = dynamic_cast<CNewParticleEffect*>(info.m_pRenderable);
			if (pEffect)
			{
				Q_snprintf(pTemp, sizeof(pTemp), "ps: %s", pEffect->GetName());
				pClassName = pTemp;
			}
			else if (dynamic_cast<CParticleEffectBinding*>(info.m_pRenderable))
			{
				pClassName = "<old particle system>";
			}
		}

		con_nprint_t np;
		np.time_to_live = 0.1f;
		np.fixed_width_font = true;
		np.color[0] = 1.0;
		np.color[1] = 0.8;
		np.color[2] = 0.1;
		np.index = m_nDebugIndex++;

		engine->Con_NXPrintf(&np, "%s", pClassName);
	}

	if (list.pHead)
	{
		m_DeferredInserts.PushItem(list);
		int nLeaves = GetRenderableLeaves(handle, leafList);
		m_Renderables[handle].m_Area = engine->GetLeavesArea(leafList, nLeaves);
	}
}

//-----------------------------------------------------------------------------
// Removes an element from the tree
//-----------------------------------------------------------------------------
void CClientLeafSystem::RemoveFromTree( ClientRenderHandle_t& handle )
{
	m_RenderablesInLeaf.RemoveElement( handle );

	// Remove all shadows cast onto the object
	m_ShadowsOnRenderable.RemoveBucket( handle );

	RenderableInfo_t const& info = m_Renderables[handle];

	// If the renderable is a brush model, then remove all shadows from it
	if (info.m_Flags & RENDER_FLAGS_BRUSH_MODEL)
	{
		g_pClientShadowMgr->RemoveAllShadowsFromReceiver( 
			info.m_pRenderable, SHADOW_RECEIVER_BRUSH_MODEL );
	}
	else if(info.m_Flags & RENDER_FLAGS_STUDIO_MODEL )
	{
		g_pClientShadowMgr->RemoveAllShadowsFromReceiver( 
			info.m_pRenderable, SHADOW_RECEIVER_STUDIO_MODEL );
	}
	else if (info.m_Flags & RENDER_FLAGS_STATIC_PROP)
	{
		g_pClientShadowMgr->RemoveAllShadowsFromReceiver(
			info.m_pRenderable, SHADOW_RECEIVER_STATIC_PROP);
	}
}


//-----------------------------------------------------------------------------
// Call this when the renderable moves
//-----------------------------------------------------------------------------
void CClientLeafSystem::RenderableChanged( ClientRenderHandle_t handle )
{
	if (m_bDisableLeafReinsertion)
	{
		DevWarning("Renderable %d re-entrant after frame!\n", handle);
	}

	Assert ( handle != INVALID_CLIENT_RENDER_HANDLE );
	Assert( m_Renderables.IsValidIndex( handle ) );
	if ( !m_Renderables.IsValidIndex( handle ) )
		return;

	RenderableInfo_t& info = m_Renderables[handle];
	if ((info.m_Flags & RENDER_FLAGS_HASCHANGED) == 0)
	{
		AUTO_LOCK(m_DirtyRenderablesMutex);
		info.m_Flags &= ~RENDER_FLAGS_BOUNDS_VALID;
		info.m_Flags |= RENDER_FLAGS_HASCHANGED;
		m_DirtyRenderables.AddToTail(handle);
	}
	else
	{
		if (s_bIsInRecomputeRenderableLeaves)
		{
			Warning("------------------------------------------------------------\n");
			Warning("------------------------------------------------------------\n");
			Warning("------------------------------------------------------------\n");
			Warning("------------------------------------------------------------\n");
			Warning("Re-entrancy found in CClientLeafSystem::RenderableChanged\n");
			Warning("Contact mastercoms\n");
			Warning("------------------------------------------------------------\n");
			Warning("------------------------------------------------------------\n");
			Warning("------------------------------------------------------------\n");
			Warning("------------------------------------------------------------\n");
		}
		// It had better be in the list
		Assert(m_DirtyRenderables.Find(handle) != m_DirtyRenderables.InvalidIndex());
	}
}


//-----------------------------------------------------------------------------
// Returns if it's a view model render group
//-----------------------------------------------------------------------------
inline bool CClientLeafSystem::IsViewModelRenderGroup( RenderGroup_t group ) const
{
	return (group == RENDER_GROUP_VIEW_MODEL_TRANSLUCENT) || (group == RENDER_GROUP_VIEW_MODEL_OPAQUE);
}


//-----------------------------------------------------------------------------
// Adds, removes renderables from view model list
//-----------------------------------------------------------------------------
void CClientLeafSystem::AddToViewModelList( ClientRenderHandle_t handle )
{
	MEM_ALLOC_CREDIT();
	Assert( m_ViewModels.Find( handle ) == m_ViewModels.InvalidIndex() );
	m_ViewModels.AddToTail( handle );
}

void CClientLeafSystem::RemoveFromViewModelList( ClientRenderHandle_t handle )
{
	int i = m_ViewModels.Find( handle );
	Assert( i != m_ViewModels.InvalidIndex() );
	m_ViewModels.FastRemove( i );
}


//-----------------------------------------------------------------------------
// Call this to change the render group
//-----------------------------------------------------------------------------
void CClientLeafSystem::SetRenderGroup( ClientRenderHandle_t handle, RenderGroup_t group )
{
	RenderableInfo_t *pInfo = &m_Renderables[handle];

	bool twoPass = false;
	if ( group == RENDER_GROUP_TWOPASS )
	{
		twoPass = true;
		group = RENDER_GROUP_TRANSLUCENT_ENTITY;
	}

	if ( twoPass )
	{
		pInfo->m_Flags |= RENDER_FLAGS_TWOPASS;
	}
	else
	{
		pInfo->m_Flags &= ~RENDER_FLAGS_TWOPASS;
	}

	bool bOldViewModelRenderGroup = IsViewModelRenderGroup( (RenderGroup_t)pInfo->m_RenderGroup );
	bool bNewViewModelRenderGroup = IsViewModelRenderGroup( group );
	if ( bOldViewModelRenderGroup != bNewViewModelRenderGroup )
	{
		if ( bOldViewModelRenderGroup )
		{
			RemoveFromViewModelList( handle );
		}
		else 
		{
			AddToViewModelList( handle );
		}
	}

	pInfo->m_RenderGroup = group;

}


//-----------------------------------------------------------------------------
// Detail system marks 
//-----------------------------------------------------------------------------
void CClientLeafSystem::DrawDetailObjectsInLeaf( int leaf, int nFrameNumber, int& nFirstDetailObject, int& nDetailObjectCount )
{
	ClientLeaf_t &leafInfo = m_Leaf[leaf];
	leafInfo.m_DetailPropRenderFrame = nFrameNumber;
	nFirstDetailObject = leafInfo.m_FirstDetailProp;
	nDetailObjectCount = leafInfo.m_DetailPropCount;
}


//-----------------------------------------------------------------------------
// Are we close enough to this leaf to draw detail props *and* are there any props in the leaf?
//-----------------------------------------------------------------------------
bool CClientLeafSystem::ShouldDrawDetailObjectsInLeaf( int leaf, int frameNumber )
{
	ClientLeaf_t &leafInfo = m_Leaf[leaf];
	return ( (leafInfo.m_DetailPropRenderFrame == frameNumber ) &&
			 ( ( leafInfo.m_DetailPropCount != 0 ) || ( leafInfo.m_pSubSystemData[CLSUBSYSTEM_DETAILOBJECTS] ) ) );
}


#define LeafToMarker( leaf ) reinterpret_cast<RenderableInfo_t *>(( (intp)(leaf) << 1 ) | 1)
#define IsLeafMarker( p ) (bool)((reinterpret_cast<size_t>(p)) & 1)
#define MarkerToLeaf( p ) (int)((reinterpret_cast<size_t>(p)) >> 1)

//-----------------------------------------------------------------------------
// Compute which leaf the translucent renderables should render in
//-----------------------------------------------------------------------------
void CClientLeafSystem::ComputeTranslucentRenderLeaf( int count, const LeafIndex_t *pLeafList, const LeafFogVolume_t *pLeafFogVolumeList, int frameNumber, int viewID )
{
	ASSERT_NO_REENTRY();
	VPROF_BUDGET( "CClientLeafSystem::ComputeTranslucentRenderLeaf", "ComputeTranslucentRenderLeaf"  );

	// For better sorting, we're gonna choose the leaf that is closest to the camera.
	// The leaf list passed in here is sorted front to back
	bool bThreaded = (cl_threaded_client_leaf_system.GetInt() > 1 && g_pThreadPool->NumIdleThreads());
	int globalFrameCount = gpGlobals->framecount;
	int i;

	static CUtlVector<RenderableInfo_t *> orderedList; // @MULTICORE (toml 8/30/2006): will need to make non-static if thread this function
	static CUtlVector<IClientRenderable *> renderablesToUpdate;
	int leaf = 0;
	for ( i = 0; i < count; ++i )
	{
		leaf = pLeafList[i];
		orderedList.AddToTail( LeafToMarker( leaf ) );

		// iterate over all elements in this leaf
		unsigned int idx = m_RenderablesInLeaf.FirstElement(leaf);
		while (idx != m_RenderablesInLeaf.InvalidIndex())
		{
			RenderableInfo_t& info = m_Renderables[m_RenderablesInLeaf.Element(idx)];
			if ( info.m_TranslucencyCalculated != globalFrameCount || info.m_TranslucencyCalculatedView != viewID )
			{ 
				// Compute translucency
				if ( bThreaded )
				{
					renderablesToUpdate.AddToTail( info.m_pRenderable );
				}
				else
				{
					info.m_pRenderable->ComputeFxBlend();
				}
				info.m_TranslucencyCalculated = globalFrameCount;
				info.m_TranslucencyCalculatedView = viewID;
			}
			orderedList.AddToTail( &info );
			idx = m_RenderablesInLeaf.NextElement(idx); 
		}
	}

	if ( bThreaded )
	{
		ParallelProcess( "CClientLeafSystem::ComputeTranslucentRenderLeaf", renderablesToUpdate.Base(), renderablesToUpdate.Count(), &CallComputeFXBlend, &::FrameLock, &::FrameUnlock );
		renderablesToUpdate.RemoveAll();
	}

	for ( i = 0; i != orderedList.Count(); i++ )
	{
		RenderableInfo_t *pInfo = orderedList[i];
		if ( !IsLeafMarker( pInfo ) )
		{
			if( pInfo->m_RenderFrame != frameNumber )
			{   
				if( pInfo->m_RenderGroup == RENDER_GROUP_TRANSLUCENT_ENTITY )
				{
					pInfo->m_RenderLeaf = leaf;
				}
				pInfo->m_RenderFrame = frameNumber;
			}
			else if ( pInfo->m_Flags & RENDER_FLAGS_ALTERNATE_SORTING )
			{
				if( pInfo->m_RenderGroup == RENDER_GROUP_TRANSLUCENT_ENTITY )
				{
					pInfo->m_RenderLeaf = leaf;
				}
			}

		}
		else
		{
			leaf = MarkerToLeaf( pInfo );
		}
	}

	orderedList.RemoveAll();
}


//-----------------------------------------------------------------------------
// Adds a renderable to the list of renderables to render this frame
//-----------------------------------------------------------------------------
inline void AddRenderableToRenderList( CClientRenderablesList &renderList, IClientRenderable *pRenderable, 
	int iLeaf, RenderGroup_t group,	ClientRenderHandle_t renderHandle, bool bTwoPass = false )
{
#ifdef _DEBUG
	if (cl_drawleaf.GetInt() >= 0)
	{
		if (iLeaf != cl_drawleaf.GetInt())
			return;
	}
#endif

	Assert( group >= 0 && group < RENDER_GROUP_COUNT );
	
	int &curCount = renderList.m_RenderGroupCounts[group];
	if ( curCount < CClientRenderablesList::MAX_GROUP_ENTITIES )
	{
		Assert( (iLeaf >= 0) && (iLeaf <= 65535) );

		CClientRenderablesList::CEntry *pEntry = &renderList.m_RenderGroups[group][curCount];
		pEntry->m_pRenderable = pRenderable;
		pEntry->m_iWorldListInfoLeaf = iLeaf;
		pEntry->m_TwoPass = bTwoPass;
		pEntry->m_RenderHandle = renderHandle;
		curCount++;
	}
	else
	{
		engine->Con_NPrintf( 10, "Warning: overflowed CClientRenderablesList group %d", group );
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : renderList - 
//			renderGroup - 
//-----------------------------------------------------------------------------
void CClientLeafSystem::CollateViewModelRenderables( CUtlVector< IClientRenderable * >& opaque, CUtlVector< IClientRenderable * >& translucent )
{
	for ( int i = m_ViewModels.Count()-1; i >= 0; --i )
	{
		ClientRenderHandle_t handle = m_ViewModels[i];
		RenderableInfo_t& renderable = m_Renderables[handle];

		// NOTE: In some cases, this removes the entity from the view model list
		renderable.m_pRenderable->ComputeFxBlend();
		
		// That's why we need to test RENDER_GROUP_OPAQUE_ENTITY - it may have changed in ComputeFXBlend()
		if ( renderable.m_RenderGroup == RENDER_GROUP_VIEW_MODEL_OPAQUE || renderable.m_RenderGroup == RENDER_GROUP_OPAQUE_ENTITY )
		{
			opaque.AddToTail( renderable.m_pRenderable );
		}
		else
		{
			translucent.AddToTail( renderable.m_pRenderable );
		}
	}
}

static RenderGroup_t DetectBucketedRenderGroup( RenderGroup_t group, float fDimension )
{
	float const arrThresholds[ 3 ] = {
		200.f,	// tree size
		80.f,	// player size
		30.f,	// crate size
	};
	Assert( ARRAYSIZE( arrThresholds ) + 1 >= RENDER_GROUP_CFG_NUM_OPAQUE_ENT_BUCKETS );
	Assert( group >= RENDER_GROUP_OPAQUE_STATIC && group <= RENDER_GROUP_OPAQUE_ENTITY );

	int bucketedGroupIndex;
	if ( RENDER_GROUP_CFG_NUM_OPAQUE_ENT_BUCKETS <= 2 ||
		fDimension >= arrThresholds[1] )
	{
		if ( RENDER_GROUP_CFG_NUM_OPAQUE_ENT_BUCKETS <= 1 ||
			fDimension >= arrThresholds[0] )
			bucketedGroupIndex = 0;
		else
			bucketedGroupIndex = 1;
	}
	else
	{
		if ( RENDER_GROUP_CFG_NUM_OPAQUE_ENT_BUCKETS <= 3 ||
			fDimension >= arrThresholds[2] )
			bucketedGroupIndex = 2;
		else
			bucketedGroupIndex = 3;
	}

	// Determine the new bucketed group
	RenderGroup_t bucketedGroup = RenderGroup_t( group - ( ( RENDER_GROUP_CFG_NUM_OPAQUE_ENT_BUCKETS - 1 ) - bucketedGroupIndex ) * 2 );
	Assert( bucketedGroup >= RENDER_GROUP_OPAQUE_STATIC_HUGE && bucketedGroup <= RENDER_GROUP_OPAQUE_ENTITY );
	
	return bucketedGroup;
}

void CClientLeafSystem::CollateRenderablesInLeaf( int leaf, int worldListLeafIndex,	const SetupRenderInfo_t &info )
{
	bool portalTestEnts = r_PortalTestEnts.GetBool() && !r_portalsopenall.GetBool();
	
	// Place a fake entity for static/opaque ents in this leaf
	AddRenderableToRenderList( *info.m_pRenderList, NULL, worldListLeafIndex, RENDER_GROUP_OPAQUE_STATIC, INVALID_CLIENT_RENDER_HANDLE );
	AddRenderableToRenderList( *info.m_pRenderList, NULL, worldListLeafIndex, RENDER_GROUP_OPAQUE_ENTITY, INVALID_CLIENT_RENDER_HANDLE );

	// Collate everything.
	unsigned int idx = m_RenderablesInLeaf.FirstElement(leaf);
	for ( ;idx != m_RenderablesInLeaf.InvalidIndex(); idx = m_RenderablesInLeaf.NextElement(idx) )
	{
		ClientRenderHandle_t handle = m_RenderablesInLeaf.Element(idx);
		RenderableInfo_t& renderable = m_Renderables[handle];

		// Early out on static props if we don't want to render them
		if ((!m_DrawStaticProps) && (renderable.m_Flags & RENDER_FLAGS_STATIC_PROP))
			continue;

		// Early out if we're told to not draw small objects (top view only,
		/* that's why we don't check the z component).
		if (!m_DrawSmallObjects)
		{
			CCachedRenderInfo& cachedInfo =  m_CachedRenderInfos[renderable.m_CachedRenderInfo];
			float sizeX = cachedInfo.m_Maxs.x - cachedInfo.m_Mins.x;
			float sizeY = cachedInfo.m_Maxs.y - cachedInfo.m_Mins.y;
			if ((sizeX < 50.f) && (sizeY < 50.f))
				continue;
		}*/

		Assert( m_DrawSmallObjects ); // MOTODO

		// Don't hit the same ent in multiple leaves twice.
		if ( renderable.m_RenderGroup != RENDER_GROUP_TRANSLUCENT_ENTITY )
		{
			if ( renderable.m_RenderFrame2 == info.m_nRenderFrame )
				continue;

			renderable.m_RenderFrame2 = info.m_nRenderFrame;
		}
		else // translucent
		{
			// Shadow depth skips ComputeTranslucentRenderLeaf!

			// Translucent entities already have had ComputeTranslucentRenderLeaf called on them
			// so m_RenderLeaf should be set to the nearest leaf, so that's what we want here.
			if ( renderable.m_RenderLeaf != leaf )
				continue;
		}

		unsigned char nAlpha = 255;
		if ( info.m_bDrawTranslucentObjects ) 
		{
			// Prevent culling if the renderable is invisible
			// NOTE: OPAQUE objects can have alpha == 0. 
			// They are made to be opaque because they don't have to be sorted.
			nAlpha = renderable.m_pRenderable->GetFxBlend();
			if ( nAlpha == 0 )
				continue;
		}

		Vector absMins = renderable.m_vecAbsMins;
		Vector absMaxs = renderable.m_vecAbsMaxs;
		// If the renderable is inside an area, cull it using the frustum for that area.
		if ( portalTestEnts && renderable.m_Area != -1 )
		{
			VPROF( "r_PortalTestEnts" );
			if ( !engine->DoesBoxTouchAreaFrustum( absMins, absMaxs, renderable.m_Area ) )
				continue;
		}
		else
		{
			// cull with main frustum
			if ( engine->CullBox( absMins, absMaxs ) )
				continue;
		}

		// UNDONE: UNDONE: we're testing it now.
		// UNDONE: Investigate speed tradeoffs of occlusion culling brush models too?
		//if ( renderable.m_Flags & RENDER_FLAGS_STUDIO_MODEL )
		{
			// test to see if this renderable is occluded by the engine's occlusion system
			if ( engine->IsOccluded( absMins, absMaxs ) )
				continue;
		}

#ifdef INVASION_CLIENT_DLL
		if (info.m_flRenderDistSq != 0.0f)
		{
			Vector mins, maxs;
			renderable.m_pRenderable->GetRenderBounds( mins, maxs );

			if ((maxs.z - mins.z) < 100)
			{
				Vector vCenter;
				VectorLerp( mins, maxs, 0.5f, vCenter );
				vCenter += renderable.m_pRenderable->GetRenderOrigin();

				float flDistSq = info.m_vecRenderOrigin.DistToSqr( vCenter );
				if (info.m_flRenderDistSq <= flDistSq)
					continue;
			}
		}
#endif

		if( renderable.m_RenderGroup != RENDER_GROUP_TRANSLUCENT_ENTITY )
		{
			RenderGroup_t group = (RenderGroup_t)renderable.m_RenderGroup;

			// Determine object group offset
			if ( RENDER_GROUP_CFG_NUM_OPAQUE_ENT_BUCKETS > 1 &&
				 group >= RENDER_GROUP_OPAQUE_STATIC &&
				 group <= RENDER_GROUP_OPAQUE_ENTITY )
			{
				Vector dims;
				VectorSubtract( absMaxs, absMins, dims );

				float const fDimension = MAX( MAX( fabs(dims.x), fabs(dims.y) ), fabs(dims.z) );
				group = DetectBucketedRenderGroup( group, fDimension );
				
				Assert( group >= RENDER_GROUP_OPAQUE_STATIC_HUGE && group <= RENDER_GROUP_OPAQUE_ENTITY );
			}

			AddRenderableToRenderList( *info.m_pRenderList, renderable.m_pRenderable, 
				worldListLeafIndex, group, handle);
		}
		else
		{
			bool bTwoPass = ((renderable.m_Flags & RENDER_FLAGS_TWOPASS) != 0) && ( nAlpha == 255 );	// Two pass?

			// Add to appropriate list if drawing translucent objects (shadow depth mapping will skip this)
			if ( info.m_bDrawTranslucentObjects ) 
			{
				AddRenderableToRenderList( *info.m_pRenderList, renderable.m_pRenderable, 
					worldListLeafIndex, (RenderGroup_t)renderable.m_RenderGroup, handle, bTwoPass );
			}
			
			if ( bTwoPass )	// Also add to opaque list if it's a two-pass model... 
			{
				if ((renderable.m_Flags & RENDER_FLAGS_STATIC_PROP) != 0)
				{
					RenderGroup_t group = RENDER_GROUP_OPAQUE_STATIC;
					if (RENDER_GROUP_CFG_NUM_OPAQUE_ENT_BUCKETS > 1)
					{
						Vector dims;
						VectorSubtract(absMaxs, absMins, dims);

						float const fDimension = MAX(MAX(fabs(dims.x), fabs(dims.y)), fabs(dims.z));
						group = DetectBucketedRenderGroup(group, fDimension);

						Assert(group >= RENDER_GROUP_OPAQUE_STATIC_HUGE && group <= RENDER_GROUP_OPAQUE_ENTITY);
					}
					AddRenderableToRenderList(*info.m_pRenderList, renderable.m_pRenderable,
						worldListLeafIndex, group, handle, true);
				}
				else
				{
					AddRenderableToRenderList(*info.m_pRenderList, renderable.m_pRenderable,
						worldListLeafIndex, RENDER_GROUP_OPAQUE_ENTITY, handle, bTwoPass);
				}

			}
		}
	}

	// Do detail objects.
	// These don't have render handles!
	if ( info.m_bDrawDetailObjects && ShouldDrawDetailObjectsInLeaf( leaf, info.m_nDetailBuildFrame ) )
	{
		idx = m_Leaf[leaf].m_FirstDetailProp;
		int count = m_Leaf[leaf].m_DetailPropCount;
		while( --count >= 0 )
		{
			IClientRenderable* pRenderable = DetailObjectSystem()->GetDetailModel(idx);

			// FIXME: This if check here is necessary because the detail object system also maintains lists of sprites...
			if (pRenderable)
			{
				if( pRenderable->IsTransparent() )
				{
					if ( info.m_bDrawTranslucentObjects )	// Don't draw translucent objects into shadow depth maps
					{
						// Lots of the detail entities are invisible so avoid sorting them and all that.
						if( pRenderable->GetFxBlend() > 0 )
						{
							AddRenderableToRenderList( *info.m_pRenderList, pRenderable, 
								worldListLeafIndex, RENDER_GROUP_TRANSLUCENT_ENTITY, DETAIL_PROP_RENDER_HANDLE );
						}
					}
				}
				else
				{
					AddRenderableToRenderList( *info.m_pRenderList, pRenderable, 
						worldListLeafIndex, RENDER_GROUP_OPAQUE_ENTITY, DETAIL_PROP_RENDER_HANDLE );
				}
			}
			++idx;
		}
	}
}


//-----------------------------------------------------------------------------
// Sort entities in a back-to-front ordering
//-----------------------------------------------------------------------------
void CClientLeafSystem::SortEntities( const Vector &vecRenderOrigin, const Vector &vecRenderForward, CClientRenderablesList::CEntry *pEntities, int nEntities )
{
	// Don't sort if we only have 1 entity
	if ( nEntities <= 1 )
		return;

	float dists[CClientRenderablesList::MAX_GROUP_ENTITIES];

	// First get a distance for each entity.
	int i;
	for( i=0; i < nEntities; i++ )
	{
		IClientRenderable *pRenderable = pEntities[i].m_pRenderable;
		RenderableInfo_t& renderable = m_Renderables[pEntities[i].m_RenderHandle];

		// Compute the center of the object (needed for translucent brush models)
		Vector boxcenter;
		Vector mins = renderable.m_vecAbsMins;
		Vector maxs = renderable.m_vecAbsMaxs;
		VectorAdd( mins, maxs, boxcenter );
		VectorMA( pRenderable->GetRenderOrigin(), 0.5f, boxcenter, boxcenter );

		// Compute distance...
		Vector delta;
		VectorSubtract( boxcenter, vecRenderOrigin, delta );
		dists[i] = DotProduct( delta, vecRenderForward );

		if (isnan(dists[i]))
		{
			dists[i] = 0.0f;
		}
	}

	// H-sort.
	int stepSize = 4;
	while( stepSize )
	{
		int end = nEntities - stepSize;
		for( i=0; i < end; i += stepSize )
		{
			if( dists[i] > dists[i+stepSize] )
			{
				::V_swap( pEntities[i], pEntities[i+stepSize] );
				::V_swap( dists[i], dists[i+stepSize] );

				if( i == 0 )
				{
					i = -stepSize;
				}
				else
				{
					i -= stepSize << 1;
				}
			}
		}

		stepSize >>= 1;
	}
}

void CClientLeafSystem::ComputeBounds(RenderableInfo_t*& pInfo)
{
	if (pInfo->m_Flags & RENDER_FLAGS_DISABLE_RENDERING)
		return;

	if ((pInfo->m_Flags & RENDER_FLAGS_BOUNDS_VALID) != 0)
	{
#ifdef _DEBUG
		// If these assertions trigger, it means there's some state that GetRenderBounds
		// depends on which, on change, doesn't call ClientLeafSystem::RenderableChanged().
		Vector vecTestMins, vecTestMaxs;
		CalcRenderableWorldSpaceAABB(pInfo->m_pRenderable, vecTestMins, vecTestMaxs);
		AssertMsg(
			VectorsAreEqual(vecTestMins, pInfo->m_vecAbsMins, 1e-3)
			&& VectorsAreEqual(vecTestMaxs, pInfo->m_vecAbsMaxs, 1e-3),
			"Class %s changed mins/maxes w/o calling ClientLeafSystem::RenderableChanged",
			typeid(*pInfo->m_pRenderable).name()
		);
#endif
		return;
	}

	RenderableInfo_t& info = *pInfo;
	CalcRenderableWorldSpaceAABB(info.m_pRenderable, info.m_vecPendingBloatedAbsMins, info.m_vecPendingBloatedAbsMaxs);
	info.m_vecAbsMins = info.m_vecPendingBloatedAbsMins;
	info.m_vecAbsMaxs = info.m_vecPendingBloatedAbsMaxs;
	info.m_Flags |= RENDER_FLAGS_BOUNDS_VALID;
	CalcRenderableWorldSpaceAABB_Bloated(info, info.m_vecPendingBloatedAbsMins, info.m_vecPendingBloatedAbsMaxs);
}

void CClientLeafSystem::BuildRenderablesList( const SetupRenderInfo_t &info )
{
	VPROF_BUDGET( "BuildRenderablesList", "BuildRenderablesList" );
	int leafCount = info.m_pWorldListInfo->m_LeafCount;
	const Vector &vecRenderOrigin = info.m_vecRenderOrigin;
	const Vector &vecRenderForward = info.m_vecRenderForward;
	CClientRenderablesList::CEntry *pTranslucentEntries = info.m_pRenderList->m_RenderGroups[RENDER_GROUP_TRANSLUCENT_ENTITY];
	int &nTranslucentEntries = info.m_pRenderList->m_RenderGroupCounts[RENDER_GROUP_TRANSLUCENT_ENTITY];

	for( int i = 0; i < leafCount; i++ )
	{
		int nTranslucent = nTranslucentEntries;

		// Add renderables from this leaf...
		CollateRenderablesInLeaf( info.m_pWorldListInfo->m_pLeafList[i], i, info );

		int nNewTranslucent = nTranslucentEntries - nTranslucent;
		if( (nNewTranslucent != 0 ) && info.m_bDrawTranslucentObjects )
		{
			// Sort the new translucent entities.
			SortEntities( vecRenderOrigin, vecRenderForward, &pTranslucentEntries[nTranslucent], nNewTranslucent );
		}
	}
}
