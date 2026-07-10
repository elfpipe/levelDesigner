// pellsBawl_LevelDesigner_manual_paint_Qt6.cpp
// Single-file, manual-render level designer for pellsBawl (Qt6/C++)
// No QGraphicsView — we do our own paintEvent with full control over zoom/pan and hit-testing.
//
// Features:
//  - Two layers: Interaction (collision shapes) and Graphics (sprites) with visibility toggles.
//  - Draw shapes (Rect, TriLeft, TriRight) in Shapes Edit mode.
//  - Import sprites (PNG/JPG/etc.), move/rotate/scale in Graphics Edit mode.
//  - Proper transform gizmo (Move/Rotate/Scale) drawn and handled manually.
//  - View controls: Ctrl+Wheel zoom, Ctrl+Drag pan. Scene >> window.
//  - JSON Save/Load (stores shapes + sprites with pos/rot/scale and geometry).
//  - Single file. Quick & dirty but solid.
//
// Build (qmake quick):
//   qmake -project "QT += widgets" && qmake && make
// Build (CMake): standard Qt6 Widgets setup.

#include <QtWidgets>
#include <cmath>
#include <QActionGroup>
// ---------- Data Model ----------

enum class EditMode { Draw, Select, World, Window };
enum class DrawTool { Rect, TriLeft, TriRight, Image};
enum class GizmoMode { Move, Rotate, Scale };

typedef QString Id;

struct Transform {
    QPointF pos{0,0};
    qreal rotation = 0.0; // degrees
    qreal scaleX = 1.0, scaleY = 1.0;    // uniform
};

struct InteractionItem {
    Id id; // unique
    QString shapeKind; // "rect" | "tri_left" | "tri_right"
    bool isWall = false;
    // Transform tf;
    QRectF rect; // used if rect
    // bool triLeft = false;   // used if tri
};

struct SpriteItem {
    Id id;
    QString path;   // disk path
    QImage img;     // loaded image
    Transform tf;
    qreal z = 0;    // for future sorting
};

// ---------- Utility ----------
static Id newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
static qreal clamp(qreal v, qreal lo, qreal hi){ return std::max(lo, std::min(hi, v)); }

// ---------- Canvas Widget ----------
class LevelCanvas : public QWidget {
    Q_OBJECT
public:
    explicit LevelCanvas(QWidget* parent, QJsonDocument &doc, QJsonObject &root) : QWidget(parent), m_doc(doc), m_root(root) {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(false);
    }

    // Scene data
    QList<InteractionItem> interactions;
    QList<SpriteItem> sprites;

    QString spritePath;
    QImage spriteImage;
    QString jsonPath;

    // View state
    qreal zoom = 1.0;        // pixels per scene unit
    QPointF viewOrigin{0,0}; // scene coords at top-left of widget

    QRectF world{0, 0, 1200, 900};
    QRectF window{0, 0, 800, 600};

    // Modes
    EditMode edit = EditMode::Draw;
    DrawTool tool = DrawTool::Rect; // for Shapes drawing
    GizmoMode gizmo = GizmoMode::Move; // for selected item

    // Layer visibility
    bool showInteraction = true;
    bool showGraphics = true;
    bool wallMode = false; // flag for new shapes

    // Selection
    std::optional<Id> selection; // id of selected item of the active kind

    // Drawing in progress (Shapes mode)
    bool drawing = false;
    QPointF dragStartScene;
    QRectF draggedRect;

    // Panning
    bool panning = false; QPoint panStartPos; QPointF panStartOrigin;

    // Gizmo drag
    enum class Hit { None, Inside, Edge, MoveX, MoveY, RotateRing, ScaleCornerTL, ScaleCornerTR, ScaleCornerBL, ScaleCornerBR };
    Hit gizmoHit = Hit::None; QPointF gizmoStartScene; Transform startTf; QPointF startOffset; QSizeF startSize; QRectF startRect; // for rect/tri/sprite
    Qt::Corner scaleCorner = Qt::TopLeftCorner;

    QJsonDocument &m_doc;
    QJsonObject &m_root;
    bool isNew = true;

    // API helpers
    void setEditMode(EditMode m){ edit=m; clearSelection(); update(); }
    void setDrawTool(DrawTool t){ tool=t; update(); }
    void setGizmoMode(GizmoMode m){ gizmo=m; update(); }

    // Visibility
    void setInteractionVisible(bool v){ showInteraction=v; update(); }
    void setGraphicsVisible(bool v){ showGraphics=v; update(); }
    void setWallMode(bool v){ wallMode=v; }

    // Import sprite
    void importSprite(const QString& path){
        if(!QFile::exists(path)) {
            QDir d(jsonPath);
            QFile f(path);
            spritePath = d.absoluteFilePath(jsonPath) + QStringLiteral("/") + f.fileName();
        } else
            spritePath = path;
        qDebug() << "SpritePath: " << spritePath;
        spriteImage = QImage(path);
    }

    void createSprite(const QRectF&rect){
        if (spriteImage.isNull()) return;
        SpriteItem s; s.id=newId(); s.path=spritePath; s.img=spriteImage; s.tf.pos = rect.center(); s.tf.scaleX = rect.size().width() / s.img.size().width(); s.tf.scaleY = rect.size().height() / s.img.size().height(); s.z=0;
        sprites.push_back(s); selection=s.id; update();
    }

    // Save/Load JSON
    QJsonDocument toJson() const {
      if (isNew)
        QJsonObject m_root = m_doc.object();
      QJsonArray interArr;
      for (const auto& it : interactions){
          QJsonObject o; o["id"]=it.id; o["shape"]=it.shapeKind; o["is_wall"]=it.isWall;
          o["rect"]=rectToJson(it.rect);
          interArr.push_back(o);
      }
      m_root["interaction"] = interArr;
      QJsonArray sprArr;
      for (const auto& s : sprites){
          QJsonObject o; o["id"]=s.id; o["path"]=s.path; o["z"]=s.z; o["pos"]=pointToJson(s.tf.pos); o["rotation"]=s.tf.rotation; o["scaleX"]=s.tf.scaleX; o["scaleY"]=s.tf.scaleY;
          sprArr.push_back(o);
      }
      m_root["graphics"]=sprArr;
      m_root["world"] = rectToJson(world);
      m_root["window"] = rectToJson(window);
      return QJsonDocument(m_root);
    }

    QString extractFilename(QString filename) {
        if(filename.contains('/'))
            return filename.last(filename.size() - filename.lastIndexOf('/') - 1);
        else
            return filename;
    }

    QString extractPath(QString filename) {
        if(filename.contains('/'))
            return filename.first(filename.lastIndexOf('/'));
        else return filename;
    }

    void fromJson(const QJsonDocument& doc){
        isNew = false;
        interactions.clear(); sprites.clear(); clearSelection();
        /*QJsonObject*/ m_root = doc.object();
        for (auto v : m_root.value("interaction").toArray()){
            QJsonObject o=v.toObject(); InteractionItem it; it.id=o.value("id").toString(newId()); it.shapeKind=o.value("shape").toString("rect"); it.isWall=o.value("is_wall").toBool(false);
            it.rect=jsonToRect(o.value("rect"));
            interactions.push_back(it);
        }
        for (auto v : m_root.value("graphics").toArray()){
            QJsonObject o=v.toObject(); SpriteItem s; s.id=o.value("id").toString(newId()); s.path=o.value("path").toString(); s.z=o.value("z").toDouble(0);
            s.tf.pos=jsonToPoint(o.value("pos")); s.tf.rotation=o.value("rotation").toDouble(0); s.tf.scaleX=o.value("scaleX").toDouble(1.0); s.tf.scaleY=o.value("scaleY").toDouble(1.0);
            if(!QFile::exists(s.path)) {
                s.path = extractPath(jsonPath) + QStringLiteral("/") + extractFilename(s.path);
                qDebug() << "path: " << s.path;
            }
            auto e = s.img.load(s.path);
            if (!s.img.isNull()) sprites.push_back(s);
            else qDebug() << "Failed to load img" << e;
        }
        world = jsonToRect(m_root.value("world"));
        window = jsonToRect(m_root.value("window"));
        update();
    }

protected:
    // --- Rendering ---
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(35,38,42));

        // draw checker grid for scene
        drawGrid(p);

        drawWorldWindowRects(p);

        if(drawing) {
            drawDottedBox(p, draggedRect);
        }

        // alpha rules when both layers on
        qreal interAlpha = (showInteraction && showGraphics)? 0.6 : 1.0;

        // draw graphics first (z not implemented heavy yet; simple order)
        if (showGraphics) {
            for (const auto& s : sprites) drawSprite(p, s);
        }
        if (showInteraction) {
            p.setOpacity(interAlpha);
            for (const auto& it : interactions) drawInteraction(p, it);
            p.setOpacity(1.0);
        }

        // selection + gizmo overlay
        // if (selection.has_value()) drawGizmo(p);

        // border
        p.setPen(QPen(QColor(60,64,68)));
        p.drawRect(rect().adjusted(0,0,-1,-1));
    }

    // --- Input ---
    void wheelEvent(QWheelEvent* e) override {
        if (e->modifiers() & Qt::ControlModifier) {
            // zoom around cursor
            QPointF before = fromScreen(e->position());
            qreal steps = e->angleDelta().y() / 120.0; // 1 per notch
            qreal factor = std::pow(1.125, steps);
            zoom = clamp(zoom * factor, 0.05, 20.0);
            QPointF after = fromScreen(e->position());
            // keep cursor anchored
            viewOrigin += (before - after);
            update(); e->accept(); return;
        }
        QWidget::wheelEvent(e);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button()==Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)) {
            panning=true; panStartPos=e->pos(); panStartOrigin=viewOrigin; setCursor(Qt::ClosedHandCursor); return;
        }

        QPointF scenePos = fromScreen(e->pos());
        if (e->button()==Qt::LeftButton) {
            if(edit == EditMode::Select) {
                qDebug() << "seect";
                // selection + gizmo hit test
                auto [hit, id] = pickAt(scenePos);
                if (!id.isEmpty()) {
                    qDebug() << "select()";
                    selection = id;
                    gizmoHit = hit; gizmoStartScene = scenePos; captureStartForActive();
                    update(); return;
                } else {
                    clearSelection();
                    update();
                    return;
                }
            } else {
                drawing = true; dragStartScene=scenePos;
            }
        }

        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        QPointF scenePos = fromScreen(e->pos());
        if (panning) {
            QPoint d = e->pos() - panStartPos; viewOrigin = panStartOrigin - QPointF(d.x()/zoom, d.y()/zoom); update(); return;
        }
        if (drawing) {
            QPointF end = fromScreen(e->pos());
            draggedRect = normRect(dragStartScene, end);
            // preview not drawn; final shape created on release with size from drag
            update(); return;
        }

        if (selection.has_value() && gizmoHit!=Hit::None) {
            applyGizmoDrag(scenePos); update(); return;
        }

        // hover effect (cursor) when over gizmo maybe
        if (selection.has_value()) {
            auto h = gizmoHitTest(scenePos);
            switch (h) {
            case Hit::MoveX: case Hit::MoveY: setCursor(Qt::SizeAllCursor); break;
            case Hit::RotateRing: setCursor(Qt::CrossCursor); break;
            case Hit::ScaleCornerTL:
                scaleCorner = Qt::TopLeftCorner; break;
            case Hit::ScaleCornerTR:
                scaleCorner = Qt::TopRightCorner; break;
            case Hit::ScaleCornerBL:
                scaleCorner = Qt::BottomLeftCorner; break;
            case Hit::ScaleCornerBR:
                scaleCorner = Qt::BottomRightCorner; break;
            case Hit::Inside: setCursor(Qt::OpenHandCursor); break;
            default: unsetCursor(); break;
            }
            if (h == Hit::ScaleCornerTL || h == Hit::ScaleCornerTR || h == Hit::ScaleCornerBL || h == Hit::ScaleCornerBR) setCursor(Qt::SizeFDiagCursor);
        } else unsetCursor();

        QWidget::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button()==Qt::LeftButton) {
            if (panning){ panning=false; unsetCursor(); }
            if (drawing) {
                QPointF end = fromScreen(e->pos());
                QRectF r = normRect(dragStartScene, end);
                if (r.width()>2 && r.height()>2) {
                    switch(edit) {
                    case EditMode::World:
                        world = r; break;
                    case EditMode::Window:
                        window = r; break;
                    case EditMode::Draw:
                        switch (tool) {
                        case DrawTool::Image:
                            createSprite(r); break;
                        case DrawTool::Rect:
                        case DrawTool::TriLeft:
                        case DrawTool::TriRight:
                            createShapeFromRect(r); break;
                        }
                    case EditMode::Select:  break;
                    }
                }
                drawing=false; update();
            }
            gizmoHit = Hit::None;
            return;
        }
        QWidget::mouseReleaseEvent(e);
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (e->key()==Qt::Key_Delete) {
            if (!selection.has_value()) return;
            for (int i=0;i<interactions.size();++i) if (interactions[i].id==selection) { interactions.removeAt(i); break; }
            for (int i=0;i<sprites.size();++i) if (sprites[i].id==selection) { sprites.removeAt(i); break; }
            clearSelection(); update(); return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    // ---- Drawing helpers ----
    void drawGrid(QPainter& p){
        // visible scene rect
        QRectF vis = { viewOrigin, QSizeF(width()/zoom, height()/zoom) };
        qreal step = 64; // scene units
        p.setPen(QPen(QColor(55,58,62)));
        for (qreal x = std::floor(vis.left()/step)*step; x<vis.right(); x+=step) p.drawLine(toScreen(QPointF(x, vis.top())), toScreen(QPointF(x, vis.bottom())));
        for (qreal y = std::floor(vis.top()/step)*step; y<vis.bottom(); y+=step) p.drawLine(toScreen(QPointF(vis.left(), y)), toScreen(QPointF(vis.right(), y)));
        // origin axes
        p.setPen(QPen(QColor(90,95,100)));
        p.drawLine(toScreen(QPointF(0, vis.top())), toScreen(QPointF(0, vis.bottom())));
        p.drawLine(toScreen(QPointF(vis.left(), 0)), toScreen(QPointF(vis.right(), 0)));
    }

    void drawDottedBox(QPainter &p, const QRectF &rect){
        p.save();
        QPen pen(QColor(150,150,180)); pen.setWidthF(1.5/zoom); p.setPen(pen);
        QColor fill = QColor(100,100,150);
        fill.setAlphaF(0.10);
        p.setBrush(fill); p.drawRect(toScreen(rect));
        p.restore();
    }

    void drawWorldWindowRects(QPainter &p){
        p.save();
        QPen pen(QColor(150,150,180)); pen.setWidthF(1.5/zoom); p.setPen(pen);
        QColor fill = QColor(100,100,150);
        fill.setAlphaF(0.30);
        p.setBrush(fill); p.drawRect(toScreen(world));
        fill = QColor(100, 150, 100);
        fill.setAlphaF(0.30);
        p.setBrush(fill); p.drawRect(toScreen(window));
        p.restore();
    }

    void drawInteraction(QPainter& p, const InteractionItem& it){
        QPen pen(QColor(0,255,0)); pen.setWidthF(1.5/zoom); p.setPen(pen);
        QColor fill = it.isWall? QColor(140,210,140) : QColor(0,200,0);
        fill.setAlphaF(0.85);
        p.save();
        if (it.shapeKind=="rect") {
            p.setBrush(fill); p.drawRect(toScreen(it.rect));
        } else if (it.shapeKind=="tri_right" || it.shapeKind=="tri_left") {
            QPainterPath path; QPolygonF tri = triLocal(toScreen(it.rect), it.shapeKind=="tri_left");
            p.setBrush(fill); p.drawPolygon(tri);
        }
        p.restore();

        if (selection==it.id) drawSelectionBounds(p, Transform(), it.rect);
    }

    void drawSprite(QPainter& p, const SpriteItem& s){
        if (s.img.isNull()) return;
        p.save(); applyTransform(p, s.tf);
        QRectF r = QRectF(-s.img.width()/2.0, -s.img.height()/2.0, s.img.width(), s.img.height());
        p.drawImage(r.topLeft(), s.img); // image not auto-scaled; transform applies scale
        p.restore();
        if (selection==s.id) drawSelectionBounds(p, s.tf, r);
    }

    void drawSelectionBounds(QPainter& p, const Transform& tf, const QRectF& localBounds){
        p.save();
        QPen selPen(QColor(255,165,0)); selPen.setWidthF(2.0/zoom); p.setPen(selPen); p.setBrush(Qt::NoBrush);
        p.save(); applyTransform(p, tf);
        p.drawRect(localBounds.adjusted(-1,-1,1,1));
        // gizmo visuals depending on mode
        if (gizmo==GizmoMode::Move) {
            // center + X/Y arrows
            p.restore();
            p.save();
            Transform M = { tf.pos, tf.rotation, 1.0, 1.0 }; applyTransform(p, M);
            QPointF c = localBounds.center();
            drawCenterDot(p, c);
            drawAxisArrow(p, c, QPointF(80,0));
            drawAxisArrow(p, c, QPointF(0,80));
        } else if (gizmo==GizmoMode::Rotate) {
            QPointF c = localBounds.center(); qreal rad = std::max(localBounds.width(), localBounds.height())*0.75;
            QPen rp(QColor(230,230,230)); rp.setWidthF(2.0/zoom); rp.setStyle(Qt::DashLine); p.setPen(rp);
            p.drawEllipse(c, rad, rad);
            p.setBrush(Qt::white); p.setPen(QPen(Qt::black, 1.0/zoom)); p.drawEllipse(c + QPointF(rad,0), 6/zoom, 6/zoom);
        } else if (gizmo==GizmoMode::Scale) {
            drawCornerBox(p, localBounds.topLeft());
            drawCornerBox(p, localBounds.topRight());
            drawCornerBox(p, localBounds.bottomLeft());
            drawCornerBox(p, localBounds.bottomRight());
        }
        p.restore();
        p.restore();
    }

    void drawCenterDot(QPainter& p, const QPointF& c){ p.setBrush(Qt::white); p.setPen(QPen(Qt::black, 1.0/zoom)); p.drawEllipse(c, 6/zoom, 6/zoom); }
    void drawAxisArrow(QPainter& p, const QPointF& c, const QPointF& dir){
        QPen ax(QColor(230,230,230)); ax.setWidthF(2.0/zoom); p.setPen(ax);
        p.drawLine(c, c+dir);
        QPointF tip = c+dir;
        QPointF a = tip - QPointF(dir.y(), -dir.x())*0.1; // crude arrow
        QPointF b = tip + QPointF(dir.y(), -dir.x())*0.1;
        p.drawLine(tip, a); p.drawLine(tip, b);
    }
    void drawCornerBox(QPainter& p, const QPointF& c){ p.setBrush(Qt::white); p.setPen(QPen(Qt::black, 1.0/zoom)); p.drawRect(QRectF(c-QPointF(6/zoom,6/zoom), QSizeF(12/zoom,12/zoom))); }

    // ---- Geometry helpers ----
    QPolygonF triLocal(const QRectF& R, bool left) const {
        if (left) return QPolygonF{ R.topRight(), R.bottomLeft(), R.bottomRight() };
        return QPolygonF{ R.topLeft(), R.bottomRight(), R.bottomLeft() };
    }

    // ---- Transform helpers ----
    void applyTransform(QPainter& p, const Transform& tf){ p.translate(toScreen(tf.pos)); p.rotate(tf.rotation); p.scale(zoom*tf.scaleX, zoom*tf.scaleY); }
    QPointF toScreen(const QPointF& scene) const { QPointF rel = scene - viewOrigin; return QPointF(rel.x()*zoom, rel.y()*zoom); }
    QRectF toScreen(const QRectF& scene) const { QPointF sc1 = toScreen(scene.topLeft()); QPointF sc2 = toScreen(scene.bottomRight()); QRectF r; r.setTopLeft(sc1); r.setBottomRight(sc2); return r; }
    QPointF fromScreen(const QPointF& screen) const { return viewOrigin + QPointF(screen.x()/zoom, screen.y()/zoom); }

    QRectF normRect(const QPointF& a, const QPointF& b){ return QRectF(QPointF(std::min(a.x(), b.x()), std::min(a.y(), b.y())), QPointF(std::max(a.x(), b.x()), std::max(a.y(), b.y()))); }

    // ---- Creation ----
    void createShapeFromRect(const QRectF& r){
        InteractionItem it; it.id=newId(); it.isWall=wallMode; it.rect =r;
        if (tool==DrawTool::Rect){ it.shapeKind="rect"; }
        else { it.shapeKind=(tool==DrawTool::TriLeft?"tri_left":"tri_right"); }
        interactions.push_back(it); selection = it.id; update();
    }

    // ---- Picking & Gizmo ----
    std::pair<Hit, Id> pickAt(const QPointF& scenePos){
        // prefer active kind; check gizmo first
        if (selection.has_value()){
            Hit h = gizmoHitTest(scenePos);
            if (h!=Hit::None) return {h, *selection};
        }
        if (showInteraction) {
            for (int i=sprites.size()-1; i>=0; --i){ // top-most last
                if (containsSprite(sprites[i], scenePos)) return {Hit::Inside, sprites[i].id};
            }
        }
        if (showGraphics){
            for (int i=interactions.size()-1; i>=0; --i){ if (containsInteraction(interactions[i], scenePos)) return {Hit::Inside, interactions[i].id}; }
        }
        return {Hit::None, Id()};
    }

    bool containsSprite(const SpriteItem& s, const QPointF& scenePos){
        if (s.img.isNull()) return false;
        QTransform M; M.translate(s.tf.pos.x(), s.tf.pos.y()); M.rotate(s.tf.rotation); M.scale(s.tf.scaleX, s.tf.scaleY);
        QRectF r(-s.img.width()/2.0, -s.img.height()/2.0, s.img.width(), s.img.height());
        QPointF local = M.inverted().map(scenePos);
        return r.contains(local);
    }

    bool containsInteraction(const InteractionItem& it, const QPointF& scenePos){
        if (it.shapeKind=="rect") return it.rect.contains(scenePos);
        else return QPolygonF(triLocal(it.rect, it.shapeKind=="tri_left")).containsPoint(scenePos, Qt::OddEvenFill);
    }

    void captureStartForActive(){
        if (!selection.has_value()) return;
        auto* s = findSprite(*selection); if (s) { startTf = s->tf; startSize = QSizeF(s->img.width(), s->img.height()); }
        auto* it = findInteraction(*selection); if (it) { startTf = {}; startRect = it->rect; startOffset = it->rect.topLeft() - gizmoStartScene; startSize = it->rect.size(); }
    }

    Hit gizmoHitTest(const QPointF& scenePos){
        if (!selection.has_value()) return Hit::None;
        QRectF b; Transform tf;
        auto* s=findSprite(*selection); if(s) { b = QRectF(-s->img.width()/2.0, -s->img.height()/2.0, s->img.width(), s->img.height()); tf=s->tf; }
        auto* it=findInteraction(*selection); if(it) { b = (*it).rect; tf={}; }
        if (!s && !it) return Hit::None;

        // map scene point to local
        QTransform M; M.translate(tf.pos.x(), tf.pos.y()); M.rotate(tf.rotation); M.scale(tf.scaleX, tf.scaleY);
        QPointF q = M.inverted().map(scenePos);
        // test by mode
        if (gizmo==GizmoMode::Move){
            // X axis line and Y axis line near tolerance
            qreal tol = 8.0 / zoom; QPointF c = b.center();
            if (distToSegment(q, c, c+QPointF(80,0)) < tol) return Hit::MoveX;
            if (distToSegment(q, c, c+QPointF(0,80)) < tol) return Hit::MoveY;
            if (b.adjusted(-10,-10,10,10).contains(q)) return Hit::Inside; // drag anywhere
            return Hit::None;
        } else if (gizmo==GizmoMode::Rotate){
            QPointF c=b.center(); qreal rad = std::max(b.width(), b.height())*0.75; qreal d=QLineF(q,c).length(); if (std::abs(d-rad) < 10.0/zoom) return Hit::RotateRing; return Hit::None;
        } else { // Scale
            qreal tol = 10.0/zoom;
            if (QRectF(b.topLeft()-QPointF(tol,tol), QSizeF(2*tol,2*tol)).contains(q)) return Hit::ScaleCornerTL;
            if (QRectF(b.topRight()-QPointF(tol,tol), QSizeF(2*tol,2*tol)).contains(q)) return Hit::ScaleCornerTR;
            if (QRectF(b.bottomLeft()-QPointF(tol,tol), QSizeF(2*tol,2*tol)).contains(q)) return Hit::ScaleCornerBL;
            if (QRectF(b.bottomRight()-QPointF(tol,tol), QSizeF(2*tol,2*tol)).contains(q)) return Hit::ScaleCornerBR;
            return Hit::None;
        }
    }

    void applyGizmoDrag(const QPointF& scenePos){
        if (!selection.has_value()) return;
        if (auto* s = findSprite(*selection)) applyGizmoTo(tfRef(s->tf), scenePos);
        if (auto* it = findInteraction(*selection)) applyGizmoTo(it, scenePos);
    }

    Transform& tfRef(Transform& t){ return t; }

    void applyGizmoTo(Transform& tf, const QPointF& scenePos){
        QPointF delta = scenePos - gizmoStartScene;
        if (gizmo==GizmoMode::Move){
            if (gizmoHit==Hit::MoveX) tf.pos = startTf.pos + QPointF(delta.x(), 0);
            else if (gizmoHit==Hit::MoveY) tf.pos = startTf.pos + QPointF(0, delta.y());
            else tf.pos = startTf.pos + delta;
        } else if (gizmo==GizmoMode::Rotate){
            QPointF c = startTf.pos; QLineF a(c, gizmoStartScene), b(c, scenePos); qreal da = a.angleTo(b); tf.rotation = startTf.rotation + da;
        } else if (gizmo==GizmoMode::Scale){
            // uniform scaling from center
            QPointF c = startTf.pos; qreal d0 = QLineF(c, gizmoStartScene).length(); qreal d1 = QLineF(c, scenePos).length(); if (d0>0) { tf.scaleX = clamp(startTf.scaleX * (d1/d0), 0.05, 100.0); tf.scaleY = clamp(startTf.scaleY * (d1/d0), 0.05, 100.0); }
        }
    }

    QRectF scaleRect(QRectF rect, double factor) {
        return QRectF(rect.center() - factor * (rect.bottomRight() - rect.center()), factor * rect.size());
    }

    void applyGizmoTo(InteractionItem * it, const QPointF& scenePos){
        QPointF delta = scenePos - gizmoStartScene;
        if (gizmo==GizmoMode::Move){
            if (gizmoHit==Hit::MoveX) it->rect.moveLeft(startRect.left() + delta.x());
            else if (gizmoHit==Hit::MoveY) it->rect.moveTop(startRect.top() + delta.y());
            else it->rect.moveTopLeft(startRect.topLeft() + delta);
        } else if (gizmo==GizmoMode::Scale){
            switch(scaleCorner) {
            case Qt::TopLeftCorner:
                it->rect.setTopLeft(scenePos); break;
            case Qt::TopRightCorner:
                it->rect.setTopRight(scenePos); break;
            case Qt::BottomLeftCorner:
                it->rect.setBottomLeft(scenePos); break;
            case Qt::BottomRightCorner:
                it->rect.setBottomRight(scenePos); break;
            default:break;
            }
        } // rotate : do nothing
    }
    // ---- Math helpers ----
    static qreal distToSegment(const QPointF& p, const QPointF& a, const QPointF& b){
        QPointF ab=b-a, ap=p-a; double t = QPointF::dotProduct(ap, ab) / std::max(1e-9, QPointF::dotProduct(ab, ab)); t=std::clamp(t,0.0,1.0); QPointF proj=a + ab*t; return QLineF(p,proj).length();
    }

    // ---- Accessors ----
    InteractionItem* findInteraction(const Id& id){ for (auto& it : interactions) if (it.id==id) return &it; return nullptr; }
    SpriteItem* findSprite(const Id& id){ for (auto& s : sprites) if (s.id==id) return &s; return nullptr; }

    void clearSelection(){ selection.reset(); gizmoHit=Hit::None; }

    // ---- JSON helpers ----
    static QJsonObject rectToJson(const QRectF& r){ QJsonObject o; o["x"]=r.x(); o["y"]=r.y(); o["w"]=r.width(); o["h"]=r.height(); return o; }
    static QRectF jsonToRect(const QJsonValue& v){ QJsonObject o=v.toObject(); return QRectF(o.value("x").toDouble(), o.value("y").toDouble(), o.value("w").toDouble(), o.value("h").toDouble()); }
    static QJsonObject pointToJson(const QPointF& p){ QJsonObject o; o["x"]=p.x(); o["y"]=p.y(); return o; }
    static QPointF jsonToPoint(const QJsonValue& v){ QJsonObject o=v.toObject(); return QPointF(o.value("x").toDouble(), o.value("y").toDouble()); }
    static QJsonObject sizeToJson(const QSizeF& s){ QJsonObject o; o["w"]=s.width(); o["h"]=s.height(); return o; }
    static QSizeF jsonToSize(const QJsonValue& v){ QJsonObject o=v.toObject(); return QSizeF(o.value("w").toDouble(), o.value("h").toDouble()); }
};

// ---------- Main Window ----------
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(){
        canvas = new LevelCanvas(nullptr, m_doc, m_root); setCentralWidget(canvas); resize(1280,800);
        setWindowTitle("pellsBawl Level Designer — Manual Paint Qt6");
        createUi();
        statusBar()->showMessage("Ctrl+Wheel zoom | Ctrl+Drag pan | F2 Shapes / F3 Graphics | 1/2/3 tools | M/R/S gizmo");
    }

private slots:
    void onImport(){ QString p = QFileDialog::getOpenFileName(this, "Import image", {}, "Images (*.png *.jpg *.jpeg *.bmp *.gif)"); if (!p.isEmpty()) canvas->importSprite(p); }
    void onSave(){ QString p = QFileDialog::getSaveFileName(this, "Save level", {}, "Level (*.json)"); if (p.isEmpty()) return; QFile f(p); if (f.open(QIODevice::WriteOnly)) { canvas->jsonPath = p; f.write(canvas->toJson().toJson(QJsonDocument::Indented)); f.close(); } }
    void onLoad(){ QString p = QFileDialog::getOpenFileName(this, "Load level", {}, "Level (*.json)"); if (p.isEmpty()) return; QFile f(p); if (!f.open(QIODevice::ReadOnly)) return; canvas->jsonPath = p; /*QJsonDocument d*/ m_doc = QJsonDocument::fromJson(f.readAll()); f.close(); canvas->fromJson(m_doc); }

    void setModeDraw(){ canvas->setEditMode(EditMode::Draw); }
    void setModeSelect(){ canvas->setEditMode(EditMode::Select); }
    void setModeWorld(){ canvas->setEditMode(EditMode::World); }
    void setModeWindow(){ canvas->setEditMode(EditMode::Window); }

    void setToolRect(){canvas->setDrawTool(DrawTool::Rect);}
    void setToolTriLeft(){canvas->setDrawTool(DrawTool::TriLeft);}
    void setToolTriRight(){canvas->setDrawTool(DrawTool::TriRight);}
    void setToolImage(){canvas->setDrawTool(DrawTool::Image);}

    void setGizmoMove(){ canvas->setGizmoMode(GizmoMode::Move); }
    void setGizmoRotate(){ canvas->setGizmoMode(GizmoMode::Rotate); }
    void setGizmoScale(){ canvas->setGizmoMode(GizmoMode::Scale); }

    void togInteraction(bool v){ canvas->setInteractionVisible(v); }
    void togGraphics(bool v){ canvas->setGraphicsVisible(v); }
    void togWall(bool v){ canvas->setWallMode(v); }

private:
    void createUi(){
        auto *tb = addToolBar("Tools"); tb->setMovable(false);
        auto *aNew = tb->addAction("New"); connect(aNew, &QAction::triggered, this, [this]{ m_doc = QJsonDocument(); m_root = m_doc.object(); canvas->interactions.clear(); canvas->sprites.clear(); canvas->update(); });
        auto *aLoad = tb->addAction("Load"); connect(aLoad, &QAction::triggered, this, &MainWindow::onLoad);
        auto *aSave = tb->addAction("Save"); connect(aSave, &QAction::triggered, this, &MainWindow::onSave);
        tb->addSeparator();
        auto *aImport = tb->addAction("Import Img"); connect(aImport, &QAction::triggered, this, &MainWindow::onImport);
        tb->addSeparator();
        // layer toggles
        auto *aShapes = tb->addAction("Shapes (0)"); aShapes->setCheckable(true); aShapes->setChecked(true); connect(aShapes, &QAction::toggled, this, &MainWindow::togInteraction);
        auto *aImages = tb->addAction("Images (9)"); aImages->setCheckable(true); aImages->setChecked(true); connect(aImages, &QAction::toggled, this, &MainWindow::togGraphics);
        QActionGroup vGr(this); vGr.addAction(aShapes); vGr.addAction(aImages);
        tb->addSeparator();
        // wall flag
        auto *aWall = tb->addAction("Wall: OFF"); aWall->setCheckable(true); connect(aWall, &QAction::toggled, this, [this,aWall](bool on){ aWall->setText(on?"Wall: ON":"Wall: OFF"); togWall(on); });
        tb->addSeparator();
        // edit modes
        auto *mDraw = tb->addAction("Draw [F1]"); connect(mDraw, &QAction::triggered, this, &MainWindow::setModeDraw); mDraw->setCheckable(true); mDraw->toggle();
        auto *mSelect = tb->addAction("Select [F2]"); connect(mSelect, &QAction::triggered, this, &MainWindow::setModeSelect); mSelect->setCheckable(true);
        auto *mWorld = tb->addAction("World Rect [F3]"); connect(mWorld, &QAction::triggered, this, &MainWindow::setModeWorld); mWorld->setCheckable(true);
        auto *mWindow = tb->addAction("Window Rect [F4]"); connect(mWindow, &QAction::triggered, this, &MainWindow::setModeWindow); mWindow->setCheckable(true);
        auto *mGr = new QActionGroup(this); mGr->setExclusive(true); mGr->addAction(mDraw); mGr->addAction(mSelect);mGr->addAction(mWorld);mGr->addAction(mWindow);
        tb->addSeparator();
        // shape tools
        auto *tRect = tb->addAction("Rect [1]"); connect(tRect, &QAction::triggered, this, &MainWindow::setToolRect); tRect->setCheckable(true); tRect->toggle();
        auto *tTriL = tb->addAction("TriL [2]"); connect(tTriL, &QAction::triggered, this, &MainWindow::setToolTriLeft); tTriL->setCheckable(true);
        auto *tTriR = tb->addAction("TriR [3]"); connect(tTriR, &QAction::triggered, this, &MainWindow::setToolTriRight); tTriR->setCheckable(true);
        auto *tImage = tb->addAction("Image [4]"); connect(tImage, &QAction::triggered, this, &MainWindow::setToolImage); tImage->setCheckable(true);
        auto *tGr = new QActionGroup(this); tGr->setExclusive(true); tGr->addAction(tRect); tGr->addAction(tTriL);tGr->addAction(tTriR);tGr->addAction(tImage);
        tb->addSeparator();
        // gizmo
        auto *gGr = new QActionGroup(this); gGr->setExclusive(true);
        auto *gMove = new QAction("Move [M]", this); gMove->setShortcut(QKeySequence(Qt::Key_M)); connect(gMove, &QAction::triggered, this, &MainWindow::setGizmoMove); gMove->setCheckable(true); gMove->setChecked(true);
        auto *gRot = new QAction("Rotate [R]", this); gRot->setShortcut(QKeySequence(Qt::Key_R)); connect(gRot, &QAction::triggered, this, &MainWindow::setGizmoRotate); gRot->setCheckable(true);
        auto *gScale = new QAction("Scale [S]", this); gScale->setShortcut(QKeySequence(Qt::Key_S)); connect(gScale, &QAction::triggered, this, &MainWindow::setGizmoScale); gScale->setCheckable(true);
        tb->addAction(gMove); tb->addAction(gRot); tb->addAction(gScale);
        gGr->addAction(gMove); gGr->addAction(gRot); gGr->addAction(gScale);

        // enum class EditMode { Draw, Select, World, Window };
        // enum class DrawTool { Rect, TriLeft, TriRight, Image};
        // enum class GizmoMode { Move, Rotate, Scale };

        // shortcuts
        // new QShortcut(QKeySequence(Qt::Key_F1), this, [mDraw]{ mDraw->toggle(); });
        // new QShortcut(QKeySequence(Qt::Key_F2), this, [mSelect]{ mSelect->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_F3), this, [mWorld]{ mWorld->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_F4), this, [mWindow]{ mWindow->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_1), this, [tRect]{ tRect->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_2), this, [tTriL]{ tTriL->toggle(); });
        // new QShortcut(QKeySequence(Qt::Key_3), this, [tTriR]{ tTriR->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_4), this, [tImage]{ tImage->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_M), this, [gMove]{ gMove->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_R), this, [gRot]{ gRot->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_S), this, [gScale]{ gScale->toggle();});
        // new QShortcut(QKeySequence(Qt::Key_0), this, [aShapes]{ aShapes->toggle(); });
        // new QShortcut(QKeySequence(Qt::Key_9), this, [aImages]{ aImages->toggle(); });
        // new QShortcut(QKeySequence(Qt::Key_F12), this, [this]{ canvas->window = {0, 0, 800, 600}; canvas->world = { 0, 0, 1920, 1080}; canvas->update(); });
        // new QShortcut(QKeySequence(Qt::Key_W), this, [aWall]{ aWall->toggle(); });
    }

    LevelCanvas* canvas=nullptr;

  private:
    QJsonDocument m_doc;
    QJsonObject m_root;
};

int main(int argc, char** argv){ QApplication app(argc, argv); MainWindow w; w.show(); return app.exec(); }

#include "main.moc"
